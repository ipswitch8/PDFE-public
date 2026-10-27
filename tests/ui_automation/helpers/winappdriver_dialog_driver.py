#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline H phase-1: WinAppDriver-based file dialog driver.

This module replaces the pywinauto-based ``file_dialog_driver`` for
Category A tests that need to interact with native Win32 shell file
dialogs (the standard ``IFileSaveDialog`` / ``IFileOpenDialog`` that
``QFileDialog::getSaveFileName()`` / ``getOpenFileName()`` default to).

Why WinAppDriver
----------------
The native shell dialog runs in a separate top-level modal window
with class ``#32770``. It's NOT inside MainWindow's Qt UIA subtree,
so ``MainWindow.child_window(...)`` cannot reach it. pywinauto's
``Desktop(backend="uia").windows()`` can find it by title, but
typing into the file-name edit is flaky on RDP / non-interactive
automation because Win 10/11 sometimes presents the field as a
ComboBox before folder enumeration completes, which silently
swallows keystrokes.

WinAppDriver speaks the W3C WebDriver protocol over HTTP and uses
UIA more reliably for these dialogs - including handling the
ComboBox/Edit transition, sending keys via ``send_keys()`` instead
of SimulateInput, and exposing a deterministic element-discovery
API. It's the Microsoft-sanctioned harness for Win32/UWP UI
automation and is listed as a sanctioned tool in PDFE's CLAUDE.md.

Service prerequisites
---------------------
1. WinAppDriver installed (default
   ``C:\\Program Files (x86)\\Windows Application Driver\\``).
2. Windows Developer Mode enabled.
3. ``WinAppDriver.exe`` running (listens on ``127.0.0.1:4723``).
4. The ``urllib`` standard library (no third-party HTTP client
   required — we speak the JSON-Wire-Protocol that WinAppDriver
   1.2.1 expects, NOT the W3C variant that newer Appium clients
   default to).

When the service is unavailable, the helper raises
:class:`WinAppDriverUnavailable`, and callers in pywinauto tests
typically catch this and ``pytest.skip`` rather than fail.

Protocol notes
--------------
WinAppDriver 1.2.1 (the latest GA, Sept 2020) speaks the legacy
JSON-Wire-Protocol — Selenium 4.x and Appium-Python-Client 3.x
removed support for it on the client side, so this module talks
to the service over plain HTTP via :mod:`urllib.request`. The
:class:`WinAppDriverSession` and :class:`WinAppDriverElement`
wrappers expose a Selenium-WebDriver-style API surface so callers
can use the familiar ``driver.find_element(by, value)`` /
``element.click()`` / ``element.send_keys(text)`` pattern.

Usage
-----
::

    from tests.ui_automation.helpers.winappdriver_dialog_driver import (
        launch_winappdriver_session,
        save_file_dialog,
        WinAppDriverUnavailable,
    )

    try:
        driver = launch_winappdriver_session(
            exe_path=str(BUILD_DIR / "PDFE.exe"),
            extra_args=[str(test_3page)],
        )
    except WinAppDriverUnavailable as exc:
        pytest.skip(f"WinAppDriver not available: {exc}")

    try:
        ok = save_file_dialog(driver, str(out_path))
        assert ok
    finally:
        driver.quit()
"""
from __future__ import annotations

import json
import os
import socket
import time
import urllib.error
import urllib.request
from typing import Any, Dict, List, Optional


# Module-level service config. The URL matches WinAppDriver's
# documented default; override with ``WINAPPDRIVER_URL`` env var if
# the service has been started on a non-default port.
_WINAPPDRIVER_URL = os.environ.get(
    "WINAPPDRIVER_URL", "http://127.0.0.1:4723"
)
_WINAPPDRIVER_HOST = "127.0.0.1"
_WINAPPDRIVER_PORT = 4723

# Native Win32 shell dialog class - same value used by Common Item
# Dialog (Vista+) Open and Save dialogs. The title strings are
# locale-dependent so we filter by class as the primary key.
_DIALOG_CLASS_NAME = "#32770"

# Title hints used as a secondary filter. We list English variants
# here; localized tests can extend via the helper kwargs.
_OPEN_TITLES = ("Open", "Open File", "Select File", "Choose File")
_SAVE_TITLES = ("Save As", "Save", "Save File")
_CONFIRM_OVERWRITE_TITLES = ("Confirm Save As", "Confirm File Replace")


# Selenium-style modifier key codes (W3C key constants). WinAppDriver
# interprets these unicode characters as modifier-down events.
class Keys:
    CONTROL = ""
    SHIFT = ""
    ALT = ""
    META = ""
    DELETE = ""
    BACKSPACE = ""
    ENTER = ""
    TAB = ""
    SPACE = " "
    ESCAPE = ""
    HOME = ""
    END = ""
    NULL = ""  # used to release modifiers


class WinAppDriverUnavailable(RuntimeError):
    """Raised when WinAppDriver is not reachable. Callers in
    pywinauto tests should catch this and ``pytest.skip``."""


class WinAppDriverError(RuntimeError):
    """Raised on protocol-level errors from WinAppDriver itself
    (HTTP 4xx/5xx, missing element, etc). Callers typically convert
    to ``TimeoutError`` at the dialog-driving boundary."""


def _service_reachable(host: str = _WINAPPDRIVER_HOST,
                       port: int = _WINAPPDRIVER_PORT,
                       timeout: float = 0.5) -> bool:
    """Return True when a TCP connection to host:port can be opened."""
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except (OSError, socket.timeout):
        return False


# PDFE-LIMITATION-WINAPPDRIVER-V0: the 2016 "v0" beta returns /status OK
# but cannot create sessions (POST /session times out). WinAppDriver 1.2.1
# (Sept 2020 GA) is the minimum supported version. Tests that drive WAD
# must assert this floor so a stale beta install surfaces as a clear skip
# message rather than an opaque session-creation timeout.
MIN_WINAPPDRIVER_VERSION = (1, 2, 1)


def parse_status_version(body: Dict[str, Any]) -> Optional[str]:
    """Extract the WinAppDriver version STRING from a ``GET /status`` body.

    Gap A1-64 — two real defects fixed here, both of which made the version
    floor silently un-enforceable:

    1. **Wrong nesting.** WinAppDriver 1.2.x answers ``/status`` with the
       build block at the TOP LEVEL::

           {"build": {"revision": "2003", "version": "1.2.2009"},
            "os": {"arch": "amd64", "name": "windows",
                   "version": "10.0.17763"}}

       The old parser looked only under ``value``, found nothing, and returned
       ``None`` on every real 1.2.x service. Because
       :func:`require_winappdriver_version` treats ``None`` as "cannot tell —
       do not skip", the ``>= 1.2.1`` floor was never actually applied. The
       docstring's claim that a missing version field "is common" on 1.2.x was
       wrong: the field is present, the parser looked in the wrong place.
       Verified against the live service on this host (1.2.2009).
    2. **``os.version`` used as the driver version.** The old loop fell through
       from ``build`` to ``os``, so a build block without a version would have
       yielded the *Windows* version (``10.0.17763``) — comfortably above any
       floor, i.e. a guaranteed false pass.

    Accepted shapes, in priority order: ``build.version`` (top level or under
    ``value``), then a top-level/``value`` ``version`` string. ``os.version``
    is NEVER accepted. Returns None when no driver version is present.
    """
    if not isinstance(body, dict):
        return None
    for scope in (body, body.get("value") if isinstance(
            body.get("value"), dict) else None):
        if not isinstance(scope, dict):
            continue
        build = scope.get("build")
        if isinstance(build, dict) and isinstance(build.get("version"), str):
            return build["version"]
    for scope in (body, body.get("value") if isinstance(
            body.get("value"), dict) else None):
        if isinstance(scope, dict) and isinstance(scope.get("version"), str):
            return scope["version"]
    return None


def get_winappdriver_version() -> Optional[tuple]:
    """Return the running WinAppDriver version as an (major, minor, patch)
    tuple, or None if it cannot be determined.

    Parsing is delegated to :func:`parse_status_version` — see that docstring
    for the shapes handled and for the two defects this replaced.
    """
    if not _service_reachable():
        return None
    try:
        r = _http("GET", "/status", timeout=2.0)
    except WinAppDriverError:
        return None
    ver_str = parse_status_version(r)
    if not ver_str:
        return None
    parts = []
    for tok in ver_str.split("."):
        digits = "".join(c for c in tok if c.isdigit())
        if not digits:
            break
        parts.append(int(digits))
    if not parts:
        return None
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts[:3])


#: Standard install locations for WinAppDriver.exe.
WINAPPDRIVER_EXE_CANDIDATES = (
    r"C:\Program Files (x86)\Windows Application Driver\WinAppDriver.exe",
    r"C:\Program Files\Windows Application Driver\WinAppDriver.exe",
)

#: Set to 1 to let the harness start an installed-but-stopped service itself.
AUTOSTART_ENV_VAR = "PDFE_AUTOSTART_WINAPPDRIVER"


def installed_winappdriver_exe() -> Optional[str]:
    """Path to an installed ``WinAppDriver.exe``, or None.

    Used to distinguish "not installed" (a provisioning problem) from
    "installed but not running" (a launcher problem) — the old skip message
    conflated them and told the operator to install software that was already
    present.
    """
    override = os.environ.get("PDFE_WINAPPDRIVER_EXE", "").strip()
    if override and os.path.isfile(override):
        return override
    for candidate in WINAPPDRIVER_EXE_CANDIDATES:
        if os.path.isfile(candidate):
            return candidate
    return None


def start_winappdriver_service(timeout: float = 15.0) -> bool:
    """Start an installed-but-stopped WinAppDriver and wait for the port.

    Returns True when the service ends up reachable. NOTE: WinAppDriver.exe
    prints "Press ENTER to exit" and terminates on stdin EOF, so it must be
    launched fully detached with no inherited stdin — a plain
    ``subprocess.Popen`` with a closed/null stdin exits immediately. Verified
    on this host (Server 2019).
    """
    if _service_reachable():
        return True
    exe = installed_winappdriver_exe()
    if not exe:
        return False
    import subprocess
    creation = 0
    if hasattr(subprocess, "DETACHED_PROCESS"):
        creation = subprocess.DETACHED_PROCESS | getattr(
            subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    try:
        subprocess.Popen(
            [exe], cwd=os.path.dirname(exe), close_fds=True,
            creationflags=creation,
            stdin=subprocess.PIPE,  # a real pipe we simply never close
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        return False
    deadline = time.time() + timeout
    while time.time() < deadline:
        if _service_reachable():
            return True
        time.sleep(0.5)
    return False


def autostart_enabled() -> bool:
    return os.environ.get(AUTOSTART_ENV_VAR, "").strip().lower() in (
        "1", "true", "yes", "on")


def require_winappdriver_version() -> tuple:
    """Ensure a supported WinAppDriver is running; return its version tuple.

    Gap A1-64. When the prerequisite is unmet the test is marked **NOT-RUN**
    (see :mod:`tests.notrun`) rather than plainly skipped, so it can never be
    read as a pass: the reason carries a machine-readable cause tag, an
    accurate diagnosis of what is actually missing on this host, and the exact
    remedy. Three distinct causes are now reported separately —

    ``winappdriver-not-installed``
        No ``WinAppDriver.exe`` on disk. Provisioning problem.
    ``winappdriver-service-down``
        Installed but nothing is listening on the port. Launcher problem —
        set ``PDFE_AUTOSTART_WINAPPDRIVER=1`` and the harness starts it.
    ``winappdriver-too-old``
        Running but below :data:`MIN_WINAPPDRIVER_VERSION`.

    The old message conflated the first two and told the operator to install
    software that was already present, which is why the two WAD suites sat at
    "skipped, must need installing" for months on a host that had 1.2.2009
    installed the whole time.
    """
    from tests.notrun import skip_not_run  # local: helper works without pytest

    if not _service_reachable() and autostart_enabled():
        start_winappdriver_service()

    if not _service_reachable():
        exe = installed_winappdriver_exe()
        if exe is None:
            skip_not_run(
                "winappdriver-not-installed",
                f"no WinAppDriver.exe found (looked in "
                f"{', '.join(WINAPPDRIVER_EXE_CANDIDATES)}) and nothing is "
                f"listening on {_WINAPPDRIVER_HOST}:{_WINAPPDRIVER_PORT}",
                remedy=("install WinAppDriver >= "
                        f"{'.'.join(map(str, MIN_WINAPPDRIVER_VERSION))} from "
                        "https://github.com/microsoft/WinAppDriver/releases "
                        "and enable Developer Mode — see "
                        "tests/docs/winappdriver_setup.md"))
        skip_not_run(
            "winappdriver-service-down",
            f"WinAppDriver IS installed at {exe} but nothing is listening on "
            f"{_WINAPPDRIVER_HOST}:{_WINAPPDRIVER_PORT} — the service was "
            f"never started (this is a launcher problem, not a missing "
            f"install)",
            remedy=(f"start it (`\"{exe}\"`, it must keep a live stdin) or "
                    f"re-run with {AUTOSTART_ENV_VAR}=1 to have the harness "
                    f"start it"))

    ver = get_winappdriver_version()
    if ver is not None and ver < MIN_WINAPPDRIVER_VERSION:
        skip_not_run(
            "winappdriver-too-old",
            f"WinAppDriver {'.'.join(map(str, ver))} is below the minimum "
            f"supported {'.'.join(map(str, MIN_WINAPPDRIVER_VERSION))} "
            f"(PDFE-LIMITATION-WINAPPDRIVER-V0): the 2016 v0 beta cannot "
            f"create sessions",
            remedy="upgrade — see tests/docs/winappdriver_setup.md")
    return ver or MIN_WINAPPDRIVER_VERSION


def _http(method: str, path: str, body: Optional[Dict] = None,
          timeout: float = 30.0) -> Dict[str, Any]:
    """Send a JSON-Wire-Protocol request to WinAppDriver. Returns
    the parsed response dict. Raises WinAppDriverError on failure.

    The response shape is the legacy ``{"sessionId": ..., "status":
    0, "value": ...}`` format. status==0 means success; non-zero is
    an error with details in value['message'].
    """
    url = f"{_WINAPPDRIVER_URL}{path}"
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if data else {}
    req = urllib.request.Request(
        url, data=data, headers=headers, method=method,
    )
    try:
        resp = urllib.request.urlopen(req, timeout=timeout)
        body_bytes = resp.read()
    except urllib.error.HTTPError as e:
        # WinAppDriver returns rich error info in the body even on
        # 4xx/5xx; surface it.
        try:
            err_body = e.read().decode()
            err = json.loads(err_body)
            msg = err.get("value", {}).get("message", err_body)
        except Exception:
            msg = str(e)
        raise WinAppDriverError(
            f"{method} {path} -> HTTP {e.code}: {msg}"
        ) from e
    except urllib.error.URLError as e:
        raise WinAppDriverError(
            f"{method} {path} -> connection error: {e}"
        ) from e
    if not body_bytes:
        return {}
    try:
        result = json.loads(body_bytes)
    except json.JSONDecodeError as e:
        raise WinAppDriverError(
            f"{method} {path} -> invalid JSON: {e}"
        ) from e
    if isinstance(result, dict) and result.get("status", 0) != 0:
        msg = result.get("value", {})
        if isinstance(msg, dict):
            msg = msg.get("message", str(msg))
        raise WinAppDriverError(
            f"{method} {path} -> wad status "
            f"{result['status']}: {msg}"
        )
    return result if isinstance(result, dict) else {"value": result}


# WinAppDriver supports these locator strategies. We map both the
# Selenium-style names (the ones used by callers via the public
# WinAppDriverSession.find_element API) to the wire format strings.
_LOCATOR_MAP = {
    "id": "id",
    "name": "name",
    "class name": "class name",
    "xpath": "xpath",
    "tag name": "tag name",
    "link text": "link text",
    "partial link text": "partial link text",
    "accessibility id": "accessibility id",
}


class WinAppDriverElement:
    """Minimal Selenium-WebDriver-compatible element wrapper.

    Supports ``click``, ``clear``, ``send_keys``, ``find_element``,
    ``find_elements``, ``get_attribute`` and a ``text`` property.
    Created internally by :class:`WinAppDriverSession`; tests
    receive it through the find_element / find_elements API."""

    def __init__(self, session: "WinAppDriverSession", element_id: str):
        self._s = session
        self._eid = element_id

    @property
    def id(self) -> str:
        return self._eid

    def click(self) -> None:
        _http("POST",
              f"/session/{self._s.session_id}/element/{self._eid}/click",
              {})

    def clear(self) -> None:
        _http("POST",
              f"/session/{self._s.session_id}/element/{self._eid}/clear",
              {})

    def send_keys(self, value: str) -> None:
        # WinAppDriver expects {"value": [...chars...]}, where each
        # char is a single-character string.
        chars = list(value) if isinstance(value, str) else [str(value)]
        _http("POST",
              f"/session/{self._s.session_id}/element/{self._eid}/value",
              {"value": chars})

    @property
    def text(self) -> str:
        r = _http("GET",
                  f"/session/{self._s.session_id}/element/{self._eid}/text")
        v = r.get("value", "")
        return v if isinstance(v, str) else ""

    def get_attribute(self, name: str) -> Optional[str]:
        try:
            r = _http(
                "GET",
                f"/session/{self._s.session_id}/element/{self._eid}/attribute/{name}",
            )
            v = r.get("value")
            return v if v is None or isinstance(v, str) else str(v)
        except WinAppDriverError:
            return None

    def find_element(self, by: str, value: str) -> "WinAppDriverElement":
        return self._s._find_one(by, value, scope_id=self._eid)

    def find_elements(self, by: str,
                      value: str) -> List["WinAppDriverElement"]:
        return self._s._find_many(by, value, scope_id=self._eid)


class WinAppDriverSession:
    """Direct-HTTP WinAppDriver session over the JSON-Wire-Protocol.

    Mirrors the subset of Selenium WebDriver's API that the helper
    functions in this module rely on. Callers in tests typically
    use the convenience helpers (``launch_winappdriver_session``,
    ``open_file_dialog``, ``save_file_dialog``) rather than this
    class directly."""

    def __init__(self, session_id: str):
        self.session_id = session_id

    @property
    def title(self) -> str:
        r = _http("GET", f"/session/{self.session_id}/title")
        v = r.get("value")
        return v if isinstance(v, str) else (v or "")

    def quit(self) -> None:
        try:
            _http("DELETE", f"/session/{self.session_id}", timeout=5.0)
        except WinAppDriverError:
            pass

    def _find_one(self, by: str, value: str,
                  scope_id: Optional[str] = None) -> WinAppDriverElement:
        using = _LOCATOR_MAP.get(by, by)
        path = (
            f"/session/{self.session_id}/element/{scope_id}/element"
            if scope_id
            else f"/session/{self.session_id}/element"
        )
        r = _http("POST", path, {"using": using, "value": value})
        v = r.get("value", {})
        # Legacy JSON-Wire-Protocol returns {"ELEMENT": "<id>"}
        if isinstance(v, dict):
            eid = v.get("ELEMENT") or v.get(
                "element-6066-11e4-a52e-4f735466cecf"
            )
            if eid:
                return WinAppDriverElement(self, eid)
        raise WinAppDriverError(f"find_element({by!r}, {value!r}) "
                                f"returned no element")

    def _find_many(self, by: str, value: str,
                   scope_id: Optional[str] = None
                   ) -> List[WinAppDriverElement]:
        using = _LOCATOR_MAP.get(by, by)
        path = (
            f"/session/{self.session_id}/element/{scope_id}/elements"
            if scope_id
            else f"/session/{self.session_id}/elements"
        )
        try:
            r = _http("POST", path, {"using": using, "value": value})
        except WinAppDriverError:
            return []
        v = r.get("value", [])
        out: List[WinAppDriverElement] = []
        if isinstance(v, list):
            for item in v:
                if not isinstance(item, dict):
                    continue
                eid = item.get("ELEMENT") or item.get(
                    "element-6066-11e4-a52e-4f735466cecf"
                )
                if eid:
                    out.append(WinAppDriverElement(self, eid))
        return out

    def find_element(self, by: str, value: str) -> WinAppDriverElement:
        return self._find_one(by, value)

    def find_elements(self, by: str,
                      value: str) -> List[WinAppDriverElement]:
        return self._find_many(by, value)


def launch_winappdriver_session(exe_path: str,
                                extra_args: Optional[List[str]] = None,
                                app_arguments: Optional[str] = None,
                                wait_for_window: float = 8.0
                                ) -> WinAppDriverSession:
    """Launch ``exe_path`` under WinAppDriver and return a live
    :class:`WinAppDriverSession` bound to the resulting top-level
    window.

    Sends JSON-Wire-Protocol-style ``desiredCapabilities``, which is
    what WinAppDriver 1.2.1 expects. The caller must call
    ``session.quit()`` in a finally block to release the session
    and clean up the spawned process; orphaned PDFE.exe processes
    wedge subsequent runs (per CLAUDE.md guidance).

    Raises :class:`WinAppDriverUnavailable` if the service is not
    reachable on 127.0.0.1:4723.
    """
    if not _service_reachable():
        raise WinAppDriverUnavailable(
            "WinAppDriver service not reachable on "
            f"{_WINAPPDRIVER_HOST}:{_WINAPPDRIVER_PORT} - install "
            "from https://github.com/microsoft/WinAppDriver and "
            "start the service."
        )

    if app_arguments is None and extra_args:
        # WinAppDriver concatenates appArguments verbatim onto the
        # command line; quote each arg defensively so paths with
        # spaces survive the round-trip.
        quoted = [
            f'"{a}"' if (" " in a and not a.startswith('"')) else a
            for a in extra_args
        ]
        app_arguments = " ".join(quoted)

    caps: Dict[str, Any] = {
        "app": exe_path,
        "ms:waitForAppLaunch": "5",
    }
    if app_arguments:
        caps["appArguments"] = app_arguments

    try:
        r = _http("POST", "/session", {"desiredCapabilities": caps})
    except WinAppDriverError as exc:
        raise WinAppDriverUnavailable(
            f"WinAppDriver session creation failed: {exc}"
        ) from exc

    # JSON-Wire-Protocol returns sessionId at the top level.
    sid = r.get("sessionId")
    if not sid:
        # Some build variants nest it under value.sessionId.
        v = r.get("value", {})
        if isinstance(v, dict):
            sid = v.get("sessionId")
    if not sid:
        raise WinAppDriverUnavailable(
            "WinAppDriver did not return a sessionId"
        )

    session = WinAppDriverSession(sid)

    # Give the app a moment to render its main window.
    deadline = time.time() + wait_for_window
    while time.time() < deadline:
        try:
            _ = session.title
            break
        except WinAppDriverError:
            time.sleep(0.2)
    return session


def _root_session() -> WinAppDriverSession:
    """Return a Desktop-rooted session for inspecting top-level
    windows other than the one the main session is bound to.
    WinAppDriver requires a separate ``Root`` session
    (``app="Root"``) to enumerate windows owned by other processes.
    """
    try:
        r = _http("POST", "/session",
                  {"desiredCapabilities": {"app": "Root"}})
    except WinAppDriverError as exc:
        raise WinAppDriverUnavailable(
            f"WinAppDriver Root session creation failed: {exc}"
        ) from exc
    sid = r.get("sessionId")
    if not sid:
        v = r.get("value", {})
        if isinstance(v, dict):
            sid = v.get("sessionId")
    if not sid:
        raise WinAppDriverUnavailable(
            "WinAppDriver Root session did not return a sessionId"
        )
    return WinAppDriverSession(sid)


def _find_dialog_window(root: WinAppDriverSession, titles: tuple,
                        timeout: float
                        ) -> Optional[WinAppDriverElement]:
    """Find a top-level dialog (class ``#32770``).

    Win10/11 sets the QFileDialog window title to the *current
    directory path* (e.g. ``B:/``, ``Documents``), NOT a fixed
    string like ``Open`` or ``Save As``. So filtering by name is
    unreliable. We locate by class name only and pick the most
    recently appeared #32770 window.

    Implementation note: WinAppDriver's XPath parser chokes on the
    leading ``#`` in ``#32770`` (treats it as a fragment delimiter
    or attribute syntax error and times out). Use the dedicated
    ``class name`` locator strategy instead, which goes through a
    different code path that handles class strings literally.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            els = root.find_elements(
                "class name", _DIALOG_CLASS_NAME)
            if els:
                if not titles:
                    return els[-1]
                # If the caller did pass titles, prefer a match —
                # but fall back to the most recent #32770 anyway.
                for el in reversed(els):
                    try:
                        name = el.get_attribute("Name") or ""
                    except WinAppDriverError:
                        name = ""
                    for t in titles:
                        if name == t or name.startswith(t + " "):
                            return el
                return els[-1]
        except WinAppDriverError:
            pass
        time.sleep(0.2)
    return None


def _find_filename_edit(dlg: WinAppDriverElement
                        ) -> Optional[WinAppDriverElement]:
    """Locate the file-name edit control inside a Win32 file
    dialog. Tries common-dialog UIA AutomationIds first, then Name,
    then ClassName + ControlType heuristics."""
    candidates_aid = (
        "1148", "1001", "FileNameControlHost", "FileNameComboBox",
    )
    for aid in candidates_aid:
        try:
            return dlg.find_element("accessibility id", aid)
        except WinAppDriverError:
            continue
    for label in ("File name:", "File &name:", "File name"):
        try:
            return dlg.find_element("name", label)
        except WinAppDriverError:
            continue
    for ctype in ("Edit", "ComboBox"):
        try:
            return dlg.find_element("xpath", f".//{ctype}")
        except WinAppDriverError:
            continue
    return None


def _click_button_by_name(dlg: WinAppDriverElement,
                          candidates: tuple) -> bool:
    """Click the first descendant Button whose Name matches one of
    ``candidates``. Returns True on success."""
    for name in candidates:
        try:
            btn = dlg.find_element(
                "xpath", f".//Button[@Name='{name}']"
            )
            btn.click()
            return True
        except WinAppDriverError:
            continue
    return False


def _dismiss_overwrite_confirmation(root: WinAppDriverSession,
                                    timeout: float = 4.0) -> None:
    """Click Yes on a 'Confirm Save As' overwrite dialog if one
    appears. No-op when no such dialog is present within
    ``timeout``."""
    confirm = _find_dialog_window(root, _CONFIRM_OVERWRITE_TITLES,
                                  timeout)
    if confirm is None:
        return
    _click_button_by_name(confirm, ("Yes", "&Yes", "OK"))


def _clear_filename_edit(edit: WinAppDriverElement) -> None:
    """Clear the file-name control's existing text before typing a
    new path. WinAppDriver's ``send_keys`` appends rather than
    replaces, and many shell dialogs pre-populate the field with
    the last-used filename. Without an explicit clear, the typed
    path concatenates onto the residue and the shell rejects it.

    Strategy chain (each falls back on failure):
      1. ``element.clear()`` - works for plain Edit controls.
      2. Select-all via the ``Keys.CONTROL + 'a'`` chord, then
         Delete - works for ComboBox edit hosts that ignore
         ``clear()`` (Win 10/11 sometimes presents the file-name
         field as a ComboBox before folder enumeration completes).
      3. Triple-click + Delete as a final fallback.

    Failure here is swallowed; the caller surfaces malformed-path
    issues via ``TimeoutError`` from the subsequent ``send_keys``.
    """
    try:
        edit.clear()
        return
    except WinAppDriverError:
        pass
    try:
        edit.send_keys(Keys.CONTROL + "a")
        edit.send_keys(Keys.NULL)  # release modifier
        edit.send_keys(Keys.DELETE)
        return
    except WinAppDriverError:
        pass
    try:
        edit.click()
        edit.click()
        edit.click()
        edit.send_keys(Keys.DELETE)
    except WinAppDriverError:
        pass


def open_file_dialog(target_path: str,
                     timeout: float = 10.0) -> bool:
    """Drive a native Windows Open dialog: wait for it, type the
    target path into the file-name field, click Open. Returns True
    on success.

    No driver argument needed — the dialog is a top-level shell
    window enumerated via WinAppDriver's Root session. This mirrors
    the legacy pywinauto ``file_dialog_driver.open_file_dialog(pid,
    ...)`` API but uses WinAppDriver's W3C protocol underneath,
    which handles the ComboBox/Edit transition that swallows
    pywinauto keystrokes on Win 10/11.

    The architectural reason this helper takes no driver argument:
    WinAppDriver's UIA tree of Qt apps is incomplete (Qt menubars
    aren't fully exposed under the Win32 UIA path), so the proper
    pattern is pywinauto-for-in-app + WinAppDriver-for-shell-dialog.

    Raises :class:`TimeoutError` if the dialog never appears or the
    path cannot be entered. Callers typically catch and pytest.skip
    rather than failing the matrix.
    """
    root = _root_session()
    try:
        dlg = _find_dialog_window(root, _OPEN_TITLES, timeout)
        if dlg is None:
            raise TimeoutError(
                f"Open dialog did not appear within {timeout}s"
            )
        edit = _find_filename_edit(dlg)
        if edit is None:
            raise TimeoutError(
                "could not locate file-name edit in Open dialog"
            )
        try:
            edit.click()
            _clear_filename_edit(edit)
            edit.send_keys(target_path)
        except WinAppDriverError as exc:
            raise TimeoutError(
                f"could not type path into Open dialog: {exc}"
            ) from exc
        if not _click_button_by_name(dlg, ("Open", "&Open", "OK")):
            raise TimeoutError("could not click Open button")
        return True
    finally:
        root.quit()


def save_file_dialog(target_path: str,
                     timeout: float = 10.0,
                     confirm_overwrite: bool = True) -> bool:
    """Drive a native Windows Save As dialog: wait for it, type the
    target path, click Save, and optionally dismiss the overwrite
    confirmation if the destination exists.

    Returns True on success. Raises :class:`TimeoutError` if the
    dialog never appears or interaction fails.
    """
    root = _root_session()
    try:
        dlg = _find_dialog_window(root, _SAVE_TITLES, timeout)
        if dlg is None:
            raise TimeoutError(
                f"Save dialog did not appear within {timeout}s"
            )
        edit = _find_filename_edit(dlg)
        if edit is None:
            raise TimeoutError(
                "could not locate file-name edit in Save dialog"
            )
        try:
            edit.click()
            _clear_filename_edit(edit)
            edit.send_keys(target_path)
        except WinAppDriverError as exc:
            raise TimeoutError(
                f"could not type path into Save dialog: {exc}"
            ) from exc
        if not _click_button_by_name(dlg, ("Save", "&Save", "OK")):
            # Fallback: Enter is the default-button shortcut on a
            # Save As dialog. Use pyautogui to send a real
            # OS-level Enter; the WAD send_keys path on the edit
            # field doesn't always reach the dialog's accept slot
            # on this Win Server 2019 host.
            # NOTE: not focus-guarded, and the PDFE-process foreground guard
            # (tests/helpers/keys.py) deliberately does NOT apply here: this
            # dialog is the native Win32 shell dialog (class #32770), which
            # runs as a DIFFERENT process — the PDFE-PID check would wrongly
            # raise. The path was set via WinAppDriver send_keys (not
            # pyautogui), so this Enter only commits the default button.
            import pyautogui  # type: ignore
            pyautogui.PAUSE = 0.05
            pyautogui.press("enter")
            time.sleep(0.4)
        if confirm_overwrite:
            _dismiss_overwrite_confirmation(root, timeout=timeout / 2)
        return True
    finally:
        root.quit()


def cancel_file_dialog(timeout: float = 4.0) -> bool:
    """Click Cancel on whichever Open/Save dialog is currently in
    front. Useful for cleanup when a test bails out before
    completing its dialog flow. Returns True if a dialog was found
    and Cancel clicked."""
    root = _root_session()
    try:
        for titles in (_OPEN_TITLES, _SAVE_TITLES):
            dlg = _find_dialog_window(root, titles, timeout / 2)
            if dlg is not None:
                return _click_button_by_name(
                    dlg, ("Cancel", "&Cancel")
                )
        return False
    finally:
        root.quit()
