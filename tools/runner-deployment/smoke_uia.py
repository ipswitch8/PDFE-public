# -*- coding: utf-8 -*-
"""UIA smoke test for a prospective pdfe-windows runner.

WHY THIS EXISTS SEPARATELY FROM Test-PdfeRunner.ps1.

Test-PdfeRunner.ps1 proves the *tools are installed*. It cannot prove the thing
that actually goes wrong on this project's runners: that pywinauto can see and
drive a real window. A machine can have Python, pywinauto, WinAppDriver and an
interactive session recorded as present, and still be UIA-blind -- most commonly
because the desktop is locked, the RDP session was disconnected (which tears
down the visible desktop while leaving the session "interactive"), or the runner
was started as a service.

That failure is expensive precisely because it is silent: the UI suite starts,
every test times out or self-skips, and the run reports as a slow red or a
cancelled timeout rather than "this machine cannot do UI automation". Ninety
minutes of CI to learn something this script establishes in five seconds.

It drives Notepad rather than PDFE.exe on purpose: no build is required, so this
can run on a fresh machine before the repo has ever been compiled.

Usage (from an interactive desktop session on the candidate runner):
    python smoke_uia.py

Exit codes: 0 = the UIA stack works. 1 = it does not. 2 = the harness could not
run at all. Absence of a verdict is never reported as success.
"""

import sys
import time

sys.stdout.reconfigure(encoding="utf-8")

FAILURES = []


def check(label, ok, detail=""):
    status = "PASS" if ok else "FAIL"
    print("%-6s %s%s" % (status, label, (" -- " + detail) if detail else ""))
    if not ok:
        FAILURES.append(label)
    return ok


def main():
    # --- 1. interactive desktop -------------------------------------------
    try:
        import ctypes
        # Session 0 has no desktop. GetSystemMetrics(SM_CXSCREEN) returns 0
        # when there is no visible desktop to measure.
        width = ctypes.windll.user32.GetSystemMetrics(0)
        height = ctypes.windll.user32.GetSystemMetrics(1)
        check("visible desktop present",
              width > 0 and height > 0,
              "%dx%d" % (width, height))
    except Exception as exc:                                  # noqa: BLE001
        check("visible desktop present", False, str(exc))

    # --- 2. desktop not locked --------------------------------------------
    # A locked workstation keeps the session interactive but hides the desktop
    # from UIA. OpenInputDesktop fails when the desktop is locked, which is the
    # cheapest reliable probe.
    try:
        import ctypes
        hdesk = ctypes.windll.user32.OpenInputDesktop(0, False, 0x0100)
        check("desktop is unlocked", bool(hdesk),
              "OpenInputDesktop failed -- lock the session and UIA goes blind"
              if not hdesk else "")
        if hdesk:
            ctypes.windll.user32.CloseDesktop(hdesk)
    except Exception as exc:                                  # noqa: BLE001
        check("desktop is unlocked", False, str(exc))

    # --- 3. pywinauto can import and enumerate ----------------------------
    try:
        from pywinauto import Application, Desktop
    except Exception as exc:                                  # noqa: BLE001
        check("pywinauto imports", False, str(exc))
        print("\nCannot continue without pywinauto.")
        return 2
    check("pywinauto imports", True)

    try:
        windows = Desktop(backend="uia").windows()
        check("UIA can enumerate top-level windows", len(windows) > 0,
              "%d window(s) visible" % len(windows))
    except Exception as exc:                                  # noqa: BLE001
        check("UIA can enumerate top-level windows", False, str(exc))

    # --- 4. drive a real window end to end --------------------------------
    app = None
    try:
        app = Application(backend="uia").start("notepad.exe")
        time.sleep(1.5)
        dlg = app.top_window()
        dlg.wait("visible ready", timeout=20)
        check("launched and connected to a real window", True,
              "notepad: %r" % dlg.window_text())

        # Type into it and read the text back. Typing that "succeeds" without
        # the text arriving is the exact shape of a UIA-blind machine, so the
        # read-back is the assertion, not the type.
        edit = dlg.child_window(control_type="Edit")
        edit.type_keys("pdfe runner smoke", with_spaces=True)
        time.sleep(0.5)
        got = edit.get_value() if hasattr(edit, "get_value") else edit.window_text()
        check("typed text is readable back through UIA",
              "pdfe runner smoke" in (got or ""),
              "read back %r" % (got,))
    except Exception as exc:                                  # noqa: BLE001
        check("launched and connected to a real window", False, str(exc))
    finally:
        # Always tear the window down. An orphaned app is how the NEXT run gets
        # wedged -- on this project a stray pdfe.exe holds a machine-global
        # single-instance mutex, and the same discipline applies here.
        if app is not None:
            try:
                app.kill(soft=False)
            except Exception:                                 # noqa: BLE001
                pass

    # --- 5. WinAppDriver, if installed ------------------------------------
    # Absence is reported, not failed: WAD-driven tests self-skip, so a runner
    # without it is degraded rather than broken. Saying so plainly beats either
    # failing the machine or pretending the coverage is there.
    try:
        import urllib.request
        with urllib.request.urlopen("http://127.0.0.1:4723/status", timeout=3) as r:
            ok = r.status == 200
        print("%-6s WinAppDriver responding on 127.0.0.1:4723" % ("PASS" if ok else "INFO"))
    except Exception:                                         # noqa: BLE001
        print("INFO   WinAppDriver not running on 127.0.0.1:4723 "
              "(WAD-driven tests will self-skip; start it with "
              "'WinAppDriver.exe 127.0.0.1 4723' if you want that coverage)")

    print("")
    if FAILURES:
        print("UIA SMOKE TEST FAILED: %s" % ", ".join(FAILURES))
        print("This machine cannot reliably run the UI E2E suite. Registering it")
        print("as a pdfe-windows runner would produce hours-long red or cancelled")
        print("runs that look like product failures.")
        return 1
    print("UIA smoke test passed -- this machine can drive real windows.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
