# -*- coding: utf-8 -*-
"""Event-driven UI Automation listener: what a screen reader would ANNOUNCE.

WHY THIS EXISTS (GitHub #81)
----------------------------
``tests/helpers/uia_focus.py`` POLLS ``IUIAutomation::GetFocusedElement``. A
poll answers "what has focus right now", which is not the same question as
"what was announced". Anything that gains and loses focus between two polls --
a menu item the user arrowed past, a dialog that took focus for a moment -- is
simply not observable that way, and a screen-reader user would have heard every
one of them.

This module subscribes to the real notification channel instead:
``AddFocusChangedEventHandler`` (AutomationFocusChangedEvent) and, where the
comtypes binding permits it, ``AddAutomationPropertyChangedEventHandler`` for
``UIA_NamePropertyId``. Those are the same callbacks NVDA and Narrator consume,
so the ``Name`` string carried by each event is the string that would be spoken.

NO SCREEN READER IS REQUIRED, AND NONE IS INSTALLED HERE. UIA is a Windows OS
service: the provider side is Qt's accessibility bridge inside pdfe.exe and the
client side is this process. A screen reader is just another client. Callers
that want to state that as a fact rather than an assumption should assert
``screen_reader_processes() == []`` -- see ``assert_no_screen_reader()``.

THREADING
---------
pywinauto forces the importing thread into STA (``sys.coinit_flags = 2``), and
an STA client only receives COM callbacks while it pumps messages -- which a
pytest test body does not do. So the listener owns a dedicated thread that
initialises MTA, where the UIA runtime delivers callbacks on RPC pool threads
with no pump required. Events are appended under a lock and read from the test
thread.

USAGE
-----
    from tests.helpers.uia_events import UiaAnnouncementRecorder

    with UiaAnnouncementRecorder(session.pid) as rec:
        ...drive the UI...
        ann = rec.wait_for_name("File", timeout=5)
        assert ann is not None
"""
from __future__ import annotations

import ctypes
import os
import threading
import time
from typing import Callable, List, NamedTuple, Optional

# ---------------------------------------------------------------------------
# comtypes bootstrap -- same typelib GUID/path pair used by uia_focus.py
# ---------------------------------------------------------------------------

_UIA_TYPELIB = ("{944de083-8fb8-45cf-bcb7-c477acb2f897}", 1, 0)

_COINIT_MULTITHREADED = 0x0


def _load_uia_module():
    """Import the generated UIAutomationClient module (GUID, then DLL path)."""
    import comtypes.client  # noqa: PLC0415

    try:
        comtypes.client.GetModule(_UIA_TYPELIB)
    except Exception:  # noqa: BLE001
        # Fallback: bind against the DLL directly (mirrors uia_focus.py's
        # two-path bootstrap).
        dll = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"),
                           "System32", "UIAutomationCore.dll")
        comtypes.client.GetModule(dll)
    from comtypes.gen import UIAutomationClient as UIA  # noqa: PLC0415
    return UIA


class Announcement(NamedTuple):
    """One UIA notification, described the way a screen reader would read it."""

    kind: str            # "focus" or "name-changed"
    name: str            # UIA Name -- THE ANNOUNCED STRING
    automation_id: str
    class_name: str
    control_type: int
    process_id: int
    t: float             # time.monotonic() at delivery

    @property
    def short_aid(self) -> str:
        """AutomationId with Qt's ``QApplication.<parent>.`` prefix stripped."""
        return self.automation_id.rsplit(".", 1)[-1] if self.automation_id else ""


# UIA control-type ids that matter to the callers here (avoids importing the
# generated module at call sites just to name a constant).
CT_WINDOW = 50032
CT_MENU_ITEM = 50011
CT_PANE = 50033
CT_CUSTOM = 50025


class UiaAnnouncementRecorder:
    """Record UIA focus / Name-changed notifications for one process."""

    def __init__(self, pid: int, include_other_processes: bool = False):
        self.pid = int(pid)
        self.include_other_processes = include_other_processes
        self._events: List[Announcement] = []
        self._lock = threading.Lock()
        self._ready = threading.Event()
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._error: Optional[BaseException] = None
        self.property_events_available = False

    # -- lifecycle ---------------------------------------------------------

    def __enter__(self) -> "UiaAnnouncementRecorder":
        self.start()
        return self

    def __exit__(self, *_exc) -> None:
        self.stop()

    def start(self, timeout: float = 30.0) -> None:
        self._thread = threading.Thread(
            target=self._run, name="uia-announcements", daemon=True)
        self._thread.start()
        if not self._ready.wait(timeout):
            raise AssertionError(
                "UIA event listener did not become ready within %.0fs"
                % timeout)
        if self._error is not None:
            raise AssertionError(
                "UIA event listener failed to start: %r" % (self._error,))
        # The UIA runtime registers handlers asynchronously; give it a beat
        # so an event raised immediately after start() is not missed.
        time.sleep(0.5)

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=10.0)
            self._thread = None

    # -- worker ------------------------------------------------------------

    def _run(self) -> None:
        try:
            import comtypes  # noqa: PLC0415
            import comtypes.client  # noqa: PLC0415

            UIA = _load_uia_module()
            ctypes.oledll.ole32.CoInitializeEx(None, _COINIT_MULTITHREADED)

            record = self._record

            class _FocusHandler(comtypes.COMObject):
                _com_interfaces_ = [UIA.IUIAutomationFocusChangedEventHandler]

                def IUIAutomationFocusChangedEventHandler_HandleFocusChangedEvent(
                        self, sender):
                    record("focus", sender)
                    return 0

            class _PropHandler(comtypes.COMObject):
                _com_interfaces_ = [
                    UIA.IUIAutomationPropertyChangedEventHandler]

                def IUIAutomationPropertyChangedEventHandler_HandlePropertyChangedEvent(
                        self, sender, propertyId, newValue):  # noqa: N803
                    if int(propertyId) == int(UIA.UIA_NamePropertyId):
                        record("name-changed", sender)
                    return 0

            ua = comtypes.client.CreateObject(
                UIA.CUIAutomation, interface=UIA.IUIAutomation)
            focus_handler = _FocusHandler()
            ua.AddFocusChangedEventHandler(None, focus_handler)

            prop_handler = _PropHandler()
            try:
                ua.AddAutomationPropertyChangedEventHandler(
                    ua.GetRootElement(), UIA.TreeScope_Subtree, None,
                    prop_handler, [UIA.UIA_NamePropertyId])
                self.property_events_available = True
            except Exception:  # noqa: BLE001
                # Some comtypes builds expose the size_is() array parameter in
                # a shape this call does not match. Focus events alone still
                # carry the announced Name, so this is a soft capability.
                self.property_events_available = False

            self._ready.set()
            while not self._stop.is_set():
                time.sleep(0.05)

            try:
                ua.RemoveAllEventHandlers()
            except Exception:  # noqa: BLE001
                pass
        except BaseException as exc:  # noqa: BLE001
            self._error = exc
            self._ready.set()

    def _record(self, kind: str, sender) -> None:
        try:
            pid = int(sender.CurrentProcessId)
        except Exception:  # noqa: BLE001
            return
        if not self.include_other_processes and pid != self.pid:
            return
        try:
            ann = Announcement(
                kind=kind,
                name=str(sender.CurrentName or ""),
                automation_id=str(sender.CurrentAutomationId or ""),
                class_name=str(sender.CurrentClassName or ""),
                control_type=int(sender.CurrentControlType),
                process_id=pid,
                t=time.monotonic(),
            )
        except Exception:  # noqa: BLE001
            return
        with self._lock:
            self._events.append(ann)

    # -- query -------------------------------------------------------------

    def snapshot(self) -> List[Announcement]:
        with self._lock:
            return list(self._events)

    def clear(self) -> None:
        with self._lock:
            self._events.clear()

    def names(self) -> List[str]:
        return [a.name for a in self.snapshot()]

    def wait_for(self, predicate: Callable[[Announcement], bool],
                 timeout: float = 10.0,
                 poll: float = 0.1) -> Optional[Announcement]:
        """Return the first recorded announcement matching *predicate*.

        Already-recorded events are considered too, so a caller cannot lose a
        race against an event that arrived before it started waiting.
        """
        deadline = time.monotonic() + timeout
        while True:
            for ann in self.snapshot():
                if predicate(ann):
                    return ann
            if time.monotonic() >= deadline:
                return None
            time.sleep(poll)

    def wait_for_name(self, name: str, timeout: float = 10.0,
                      kind: Optional[str] = None) -> Optional[Announcement]:
        """Wait for an announcement whose Name is exactly *name*."""
        return self.wait_for(
            lambda a: a.name == name and (kind is None or a.kind == kind),
            timeout=timeout)


# ---------------------------------------------------------------------------
# Screen-reader absence probe
# ---------------------------------------------------------------------------

_SCREEN_READER_EXES = ("nvda.exe", "narrator.exe", "jfw.exe", "jaws.exe")


def screen_reader_processes() -> List[str]:
    """Return the names of any running screen-reader processes.

    Narrator ships WITH Windows (``%SystemRoot%\\System32\\Narrator.exe``
    always exists), so presence on disk proves nothing; what matters for
    GitHub #81 is that none is RUNNING to feed the events being asserted on.
    """
    found: List[str] = []
    try:
        import psutil  # noqa: PLC0415
    except Exception:  # noqa: BLE001
        return found
    for proc in psutil.process_iter(["name"]):
        try:
            nm = (proc.info.get("name") or "").lower()
        except Exception:  # noqa: BLE001
            continue
        if nm in _SCREEN_READER_EXES:
            found.append(nm)
    return sorted(set(found))


def nvda_installed() -> bool:
    """True if an NVDA installation is present on this host."""
    for base in (os.environ.get("ProgramFiles(x86)"),
                 os.environ.get("ProgramFiles"),
                 os.environ.get("ProgramData")):
        if base and os.path.exists(os.path.join(base, "NVDA", "nvda.exe")):
            return True
    return False


def assert_no_screen_reader() -> None:
    """Fail if a screen reader is running, so the events asserted on cannot
    be credited to one."""
    running = screen_reader_processes()
    assert not running, (
        "a screen reader is running (%r); GitHub #81 requires proving the "
        "UIA announcements arrive WITHOUT NVDA or Narrator, so this run "
        "cannot support that claim" % (running,))
    assert not nvda_installed(), (
        "NVDA is installed on this host; GitHub #81 requires the test to "
        "run without it so the announcements cannot be attributed to it")
