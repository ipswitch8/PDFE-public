# -*- coding: utf-8 -*-
"""
Automated UI test for CNN font matching on scanned PDFs.

Uses pywinauto to find Qt controls by objectName, clicks the Edit Text tool,
clicks a text region to trigger CNN font matching, then reads CNN debug
output to validate the results.

Instead of sleep-based timing, polls %TEMP%/pdfe_ui_status.txt for
machine-readable status signals from the PDFE application:
  TEXT_EDIT_ACTIVATING  — tool activation has begun (OCR starting)
  TEXT_EDIT_READY|N     — tool ready with N HOCR lines, awaiting span clicks
  SPAN_CLICKED|bbox     — a span click was received by the tool
  CNN_COMPLETE|family   — CNN visual ranking finished, font family chosen
  ATLAS_READY|idx|fam   — atlas built for line idx with font family

Usage: python tests/ui_test_text_edit.py [pdf_path]
"""

import sys
import os
import time
import glob
import subprocess
import tempfile
import ctypes
from ctypes import wintypes

import pyautogui
from PIL import Image

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
PDFE_EXE = r"C:\Users\dev\source\PDFE\build\vs2022\bin\Debug\pdfe.exe"
TEST_PDF = (sys.argv[1] if len(sys.argv) > 1
            else r"C:\Users\dev\NN_Netso_SpecimenC_Deposit_Letter.pdf")
SCREENSHOT_DIR = r"C:\Users\dev\source\PDFE\tests\screenshots"
TEMP_DIR = tempfile.gettempdir()  # e.g. C:\Users\...\Temp\2
CNN_DEBUG_DIR = os.path.join(TEMP_DIR, "pdfe_cnn_debug")
MATCH_DIAG_FILE = os.path.join(TEMP_DIR, "pdfe_match_diag.txt")
UI_STATUS_FILE = os.path.join(TEMP_DIR, "pdfe_ui_status.txt")

# Timeouts (seconds)
STARTUP_WAIT = 5      # for app to show window
ACTION_WAIT = 2.0     # between UI actions
STATUS_POLL_INTERVAL = 0.5  # how often to check status file
STATUS_TIMEOUT = 60.0       # max wait for any status transition

# Known serif font families for validation (sans-serif page should NOT match these)
SERIF_FAMILIES = {
    "times new roman", "georgia", "palatino", "garamond", "cambria",
    "book antiqua", "sitka", "sitka small", "sitka text", "sitka heading",
    "sitka banner", "constantia", "amiri", "gentium", "gentium basic",
    "linux libertine", "linux libertine g", "liberation serif",
    "dejavu serif", "noto serif", "source serif pro", "source serif",
    "mingLiU", "simsun", "batang",
}

# FAILSAFE off: human-abort feature that misfires on headless/RDP CI
# (cursor starts at a corner). tests/conftest.py enforces this at runtime
# too. See known_issues.md (Phase 7 sweep, run 27461377313).
pyautogui.FAILSAFE = False
pyautogui.PAUSE = 0.2


# ---------------------------------------------------------------------------
# Status file polling
# ---------------------------------------------------------------------------
def read_ui_status():
    """Read the current status from pdfe_ui_status.txt. Returns (status, detail) or (None, None)."""
    try:
        if not os.path.exists(UI_STATUS_FILE):
            return None, None
        with open(UI_STATUS_FILE, "r", encoding="utf-8") as f:
            line = f.readline().strip()
        if not line:
            return None, None
        parts = line.split("|", 1)
        status = parts[0]
        detail = parts[1] if len(parts) > 1 else ""
        return status, detail
    except (IOError, OSError):
        return None, None


def clear_ui_status():
    """Remove the status file so we can detect fresh writes."""
    try:
        if os.path.exists(UI_STATUS_FILE):
            os.remove(UI_STATUS_FILE)
    except OSError:
        pass


def wait_for_status(expected_prefix, timeout=STATUS_TIMEOUT, label=""):
    """
    Poll pdfe_ui_status.txt until the status starts with expected_prefix.
    Returns (status, detail) on success, or (None, None) on timeout.
    """
    deadline = time.time() + timeout
    desc = label or expected_prefix
    print(f"  Polling for status '{desc}' (timeout={timeout}s)...")
    last_status = None
    while time.time() < deadline:
        status, detail = read_ui_status()
        if status != last_status:
            if status:
                print(f"    Status: {status}" + (f"|{detail}" if detail else ""))
            last_status = status
        if status and status.startswith(expected_prefix):
            print(f"  Got '{status}' after {timeout - (deadline - time.time()):.1f}s")
            return status, detail
        time.sleep(STATUS_POLL_INTERVAL)
    print(f"  TIMEOUT waiting for '{desc}' after {timeout}s (last: {last_status})")
    return None, None


# ---------------------------------------------------------------------------
# Win32 helpers
# ---------------------------------------------------------------------------
def find_hwnd(title_substr):
    """Find a visible window by title substring, return HWND or None."""
    EnumWindows = ctypes.windll.user32.EnumWindows
    GetWindowTextW = ctypes.windll.user32.GetWindowTextW
    GetWindowTextLengthW = ctypes.windll.user32.GetWindowTextLengthW
    IsWindowVisible = ctypes.windll.user32.IsWindowVisible
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    found = [None]

    def cb(hwnd, _):
        if IsWindowVisible(hwnd):
            length = GetWindowTextLengthW(hwnd)
            if length > 0:
                buf = ctypes.create_unicode_buffer(length + 1)
                GetWindowTextW(hwnd, buf, length + 1)
                if title_substr.lower() in buf.value.lower():
                    found[0] = hwnd
                    return False
        return True

    EnumWindows(WNDENUMPROC(cb), 0)
    return found[0]


def bring_to_front(hwnd):
    """Force a window to the foreground using multiple Win32 techniques."""
    user32 = ctypes.windll.user32
    fg_hwnd = user32.GetForegroundWindow()
    fg_thread = user32.GetWindowThreadProcessId(fg_hwnd, None)
    cur_thread = user32.GetWindowThreadProcessId(hwnd, None)

    if fg_thread != cur_thread:
        user32.AttachThreadInput(fg_thread, cur_thread, True)

    user32.ShowWindow(hwnd, 9)   # SW_RESTORE
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    SWP_NOMOVE = 0x0002
    SWP_NOSIZE = 0x0001
    HWND_TOPMOST = -1
    HWND_NOTOPMOST = -2
    user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE)
    user32.SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE)

    if fg_thread != cur_thread:
        user32.AttachThreadInput(fg_thread, cur_thread, False)

    time.sleep(0.5)

    new_fg = user32.GetForegroundWindow()
    if new_fg == hwnd:
        print("  Window successfully brought to foreground")
    else:
        print(f"  WARNING: Failed to bring window to foreground (fg={new_fg}, target={hwnd})")


def get_window_rect(hwnd):
    """Return (left, top, right, bottom) of a window."""
    class RECT(ctypes.Structure):
        _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                     ("right", ctypes.c_long), ("bottom", ctypes.c_long)]
    rect = RECT()
    ctypes.windll.user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return rect.left, rect.top, rect.right, rect.bottom


def minimize_other_windows(keep_hwnd):
    """Minimize all windows except the given HWND to prevent focus stealing."""
    user32 = ctypes.windll.user32
    EnumWindows = user32.EnumWindows
    GetWindowTextW = user32.GetWindowTextW
    GetWindowTextLengthW = user32.GetWindowTextLengthW
    IsWindowVisible = user32.IsWindowVisible
    ShowWindow = user32.ShowWindow
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(h, _):
        if IsWindowVisible(h) and h != keep_hwnd:
            length = GetWindowTextLengthW(h)
            if length > 0:
                buf = ctypes.create_unicode_buffer(length + 1)
                GetWindowTextW(h, buf, length + 1)
                title = buf.value
                if any(t in title.lower() for t in [
                    "chrome", "firefox", "edge", "select", "font recognition",
                    "cmd", "powershell", "terminal", "bash", "code",
                    "notepad", "explorer", "indeed", "github"
                ]):
                    ShowWindow(h, 6)  # SW_MINIMIZE
        return True

    EnumWindows(WNDENUMPROC(cb), 0)
    time.sleep(0.5)


# ---------------------------------------------------------------------------
# Screenshot helper
# ---------------------------------------------------------------------------
def screenshot(name):
    """Take and save a screenshot, return path."""
    os.makedirs(SCREENSHOT_DIR, exist_ok=True)
    path = os.path.join(SCREENSHOT_DIR, f"{name}.png")
    img = pyautogui.screenshot()
    img.save(path)
    print(f"  [screenshot] {path}")
    return path


# ---------------------------------------------------------------------------
# CNN debug readers
# ---------------------------------------------------------------------------
def clear_old_debug():
    """Remove stale CNN debug files so we only see fresh results."""
    for f in [MATCH_DIAG_FILE]:
        if os.path.exists(f):
            os.remove(f)
    if os.path.isdir(CNN_DEBUG_DIR):
        for f in glob.glob(os.path.join(CNN_DEBUG_DIR, "*")):
            os.remove(f)
    clear_ui_status()
    print("  Cleared old CNN debug output and status file")


def read_match_diag():
    """Read pdfe_match_diag.txt and return its contents, or None."""
    if not os.path.exists(MATCH_DIAG_FILE):
        return None
    with open(MATCH_DIAG_FILE, "r", encoding="utf-8") as f:
        return f.read()


def read_ranking_log():
    """Read ranking_log.txt from CNN debug dir, or None."""
    path = os.path.join(CNN_DEBUG_DIR, "ranking_log.txt")
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def parse_ranking_results(diag_text):
    """Parse ranked results from diagnostic text. Returns list of dicts."""
    results = []
    if not diag_text:
        return results
    in_results = False
    for line in diag_text.splitlines():
        if "ranking results" in line.lower():
            in_results = True
            continue
        if in_results and line.strip().startswith("["):
            parts = line.strip()
            try:
                idx_end = parts.index("]")
                rest = parts[idx_end + 2:]
                name_end = rest.index(" visual=")
                family = rest[:name_end]
                scores = rest[name_end:]
                visual = float(scores.split("visual=")[1].split()[0])
                combined = float(scores.split("combined=")[1].split()[0])
                results.append({
                    "family": family,
                    "visual": visual,
                    "combined": combined,
                })
            except (ValueError, IndexError):
                continue
    return results


# ---------------------------------------------------------------------------
# Main test
# ---------------------------------------------------------------------------
def main():
    os.makedirs(SCREENSHOT_DIR, exist_ok=True)

    print("=" * 60)
    print("  PDFE CNN Font Matching - Automated UI Test")
    print("=" * 60)
    print(f"  Executable:  {PDFE_EXE}")
    print(f"  Test PDF:    {TEST_PDF}")
    print(f"  Screenshots: {SCREENSHOT_DIR}")
    print(f"  CNN debug:   {CNN_DEBUG_DIR}")
    print(f"  Status file: {UI_STATUS_FILE}")
    print()

    if not os.path.exists(PDFE_EXE):
        print(f"FAIL: Executable not found: {PDFE_EXE}")
        return 1
    if not os.path.exists(TEST_PDF):
        print(f"FAIL: Test PDF not found: {TEST_PDF}")
        return 1

    # Clear old debug output
    clear_old_debug()

    # -------------------------------------------------------------------
    # Step 1: Launch PDFE and connect pywinauto
    # -------------------------------------------------------------------
    print("\n[Step 1] Launching PDFE with test PDF...")

    proc = subprocess.Popen([PDFE_EXE, TEST_PDF])
    time.sleep(STARTUP_WAIT)

    app = None
    pwa_win = None

    try:
        from pywinauto.application import Application
        from pywinauto import timings
        timings.Timings.window_find_timeout = 15

        app = Application(backend="uia").connect(process=proc.pid, timeout=15)
        pwa_win = app.window(title_re=".*PDFE.*")
        pwa_win.wait("visible", timeout=15)
        print(f"  pywinauto connected: '{pwa_win.window_text()}'")

        pwa_win.maximize()
        time.sleep(0.5)
        pwa_win.set_focus()
        time.sleep(1.0)

        hwnd = pwa_win.handle

    except Exception as e:
        print(f"  pywinauto connect failed: {e}")
        hwnd = find_hwnd("PDFE") or find_hwnd("Deposit")
        if not hwnd:
            print("FAIL: Could not find PDFE window")
            proc.terminate()
            return 1
        bring_to_front(hwnd)

    wl, wt, wr, wb = get_window_rect(hwnd)
    print(f"  Window rect: ({wl},{wt}) - ({wr},{wb})  size={wr-wl}x{wb-wt}")
    screenshot("01_app_launched")

    # -------------------------------------------------------------------
    # Step 2: Find and click Edit Text tool, poll for TEXT_EDIT_READY
    # -------------------------------------------------------------------
    print("\n[Step 2] Activating Edit Text tool...")

    # Clear status before activation so we detect the fresh write
    clear_ui_status()

    tool_activated = False

    if pwa_win:
        # Enumerate toolbar contents for diagnostics
        try:
            toolbar = pwa_win.child_window(auto_id="annotationToolBar", control_type="ToolBar")
            if toolbar.exists(timeout=5):
                children = toolbar.children()
                print(f"  annotationToolBar: {len(children)} children")
                for i, child in enumerate(children):
                    name = child.window_text()
                    ctype = child.element_info.control_type
                    aid = child.element_info.automation_id
                    print(f"    [{i}] {ctype}: '{name}' (id='{aid}')")
        except Exception as e:
            print(f"  Toolbar enum failed: {e}")

        # Try multiple approaches to find the Edit Text button
        for search in [
            lambda: pwa_win.child_window(auto_id="textEditToolActionButton"),
            lambda: pwa_win.child_window(auto_id="textEditToolAction"),
            lambda: pwa_win.child_window(title="Edit Text", control_type="Button"),
            lambda: pwa_win.child_window(title="Edit Text", control_type="CheckBox"),
            lambda: pwa_win.child_window(title="Edit Text"),
            lambda: pwa_win.child_window(title_re=".*Edit.*Text.*"),
        ]:
            try:
                btn = search()
                if btn.exists(timeout=2):
                    print(f"  Found: '{btn.window_text()}' ({btn.element_info.control_type})")
                    btn.click_input()
                    tool_activated = True
                    screenshot("02_edit_text_clicked")
                    break
            except Exception:
                continue

    if not tool_activated:
        print("  FAIL: Could not find Edit Text button via pywinauto")
        screenshot("02_fail_no_button")
        proc.terminate()
        return 1

    # Poll for TEXT_EDIT_READY (OCR + tool setup complete)
    status, detail = wait_for_status("TEXT_EDIT_READY", timeout=90.0,
                                     label="TEXT_EDIT_READY (OCR + setup)")
    if status is None:
        print("  WARNING: TEXT_EDIT_READY not received — OCR may have failed or timed out")
        screenshot("02_timeout_no_ready")
        # Continue anyway; the tool might still work
    else:
        line_count = detail or "?"
        print(f"  Tool ready with {line_count} HOCR lines")

    screenshot("03_tool_ready")

    # -------------------------------------------------------------------
    # Step 3: Click on text to trigger span activation + CNN ranking
    # -------------------------------------------------------------------
    print("\n[Step 3] Clicking on text region to trigger CNN font matching...")

    # Minimize other windows to prevent focus stealing
    print("  Minimizing other windows...")
    minimize_other_windows(hwnd)

    # Re-focus PDFE
    ctypes.windll.user32.ShowWindow(hwnd, 3)  # SW_MAXIMIZE
    time.sleep(0.3)
    bring_to_front(hwnd)
    time.sleep(0.3)

    # Re-read window geometry after maximize
    wl, wt, wr, wb = get_window_rect(hwnd)
    win_w = wr - wl
    win_h = wb - wt
    print(f"  Window rect: ({wl},{wt}) - ({wr},{wb})  size={win_w}x{win_h}")

    def click_at(x, y, label=""):
        """Click at screen coords using pywinauto click_input (handles focus)."""
        if pwa_win:
            try:
                rect = get_window_rect(hwnd)
                rel_x = x - rect[0]
                rel_y = y - rect[1]
                if label:
                    print(f"  Clicking {label} at screen ({x},{y}), win-rel ({rel_x},{rel_y})")
                pwa_win.click_input(coords=(rel_x, rel_y))
                return
            except Exception as e:
                print(f"  pywinauto click_input failed: {e}")
        # Fallback
        bring_to_front(hwnd)
        time.sleep(0.3)
        if label:
            print(f"  Clicking {label} at ({x},{y}) [pyautogui fallback]")
        pyautogui.click(x, y)

    # Page content area: estimate the white page region.
    # In maximized window on 1920x1080: panels on left (~110px) and right (~350px),
    # toolbars at top (~55px), status bar at bottom (~50px).
    page_area_left = wl + 110
    page_area_top = wt + 55
    page_area_w = win_w - 110 - 350
    page_area_h = win_h - 55 - 50

    # "Dear Member:" is at approximately 22% from top, 32% from left of page area
    # Try multiple click positions to find a text span
    click_targets = [
        (0.32, 0.22, "Dear Member"),
        (0.35, 0.28, "paragraph line 1"),
        (0.40, 0.32, "paragraph line 2"),
        (0.30, 0.38, "paragraph line 3"),
    ]

    span_clicked = False
    for frac_x, frac_y, label in click_targets:
        text_x = page_area_left + int(page_area_w * frac_x)
        text_y = page_area_top + int(page_area_h * frac_y)

        # Clear status before click so we can detect fresh SPAN_CLICKED
        clear_ui_status()

        click_at(text_x, text_y, label)

        # Poll for SPAN_CLICKED (short timeout — if the click hit text, response is fast)
        status, detail = wait_for_status("SPAN_CLICKED", timeout=5.0,
                                         label=f"SPAN_CLICKED from '{label}'")
        if status:
            print(f"  Span click registered: {detail}")
            span_clicked = True

            # Now wait for CNN_COMPLETE or ATLAS_READY
            status2, detail2 = wait_for_status("CNN_COMPLETE", timeout=60.0,
                                               label="CNN_COMPLETE")
            if status2:
                print(f"  CNN chose font: {detail2}")
            else:
                # Maybe CNN was skipped; check for ATLAS_READY
                status3, detail3 = read_ui_status()
                if status3 and status3.startswith("ATLAS_READY"):
                    print(f"  Atlas ready (CNN may have been skipped): {detail3}")

            screenshot(f"04_span_{label.replace(' ', '_')}")
            break
        else:
            # Click didn't hit a span; check if tool is still active
            cur_status, _ = read_ui_status()
            if cur_status == "TEXT_EDIT_ACTIVATING":
                # Tool got re-activated (toggled off then on); wait for ready again
                print("  Tool re-activated — waiting for TEXT_EDIT_READY again...")
                wait_for_status("TEXT_EDIT_READY", timeout=90.0)
            screenshot(f"04_miss_{label.replace(' ', '_')}")

    if not span_clicked:
        print("  WARNING: No span click was registered after all attempts")
        print("  Trying closer inspection of page coordinates...")

        # Take a screenshot and try to identify the page bounds more precisely
        screenshot("04_no_span_all_missed")

        # Try a tighter grid of clicks in the estimated text region
        print("  Attempting grid search for clickable text...")
        for fy in [0.18, 0.20, 0.22, 0.24, 0.26]:
            for fx in [0.25, 0.30, 0.35, 0.40]:
                text_x = page_area_left + int(page_area_w * fx)
                text_y = page_area_top + int(page_area_h * fy)
                clear_ui_status()
                click_at(text_x, text_y, f"grid ({fx:.2f},{fy:.2f})")
                status, detail = wait_for_status("SPAN_CLICKED", timeout=3.0,
                                                 label="SPAN_CLICKED (grid)")
                if status:
                    print(f"  Grid click hit span at ({fx:.2f},{fy:.2f}): {detail}")
                    span_clicked = True
                    # Wait for CNN
                    wait_for_status("CNN_COMPLETE", timeout=60.0, label="CNN_COMPLETE")
                    screenshot("04_grid_hit")
                    break
                # Check if tool toggled off
                cur_status, _ = read_ui_status()
                if cur_status == "TEXT_EDIT_ACTIVATING":
                    print("  Tool re-activated mid-grid, waiting...")
                    wait_for_status("TEXT_EDIT_READY", timeout=90.0)
            if span_clicked:
                break

    # Wait a moment for any remaining writes
    time.sleep(2.0)
    screenshot("05_after_all_clicks")

    # -------------------------------------------------------------------
    # Step 4: Read and analyze CNN debug output
    # -------------------------------------------------------------------
    print("\n[Step 4] Reading CNN debug output...")

    diag = read_match_diag()
    ranking_log = read_ranking_log()

    if diag:
        print("\n  --- pdfe_match_diag.txt ---")
        for line in diag.splitlines():
            print(f"  {line}")
    else:
        print("  WARNING: No pdfe_match_diag.txt found — CNN may not have triggered")

    if ranking_log:
        lines = ranking_log.splitlines()
        print(f"\n  --- ranking_log.txt ({len(lines)} lines) ---")
        for line in lines[:40]:
            print(f"  {line}")
    else:
        print("  WARNING: No ranking_log.txt found in CNN debug dir")

    # List CNN debug files
    if os.path.isdir(CNN_DEBUG_DIR):
        files = sorted(os.listdir(CNN_DEBUG_DIR))
        print(f"\n  CNN debug files ({len(files)}):")
        for f in files[:15]:
            fpath = os.path.join(CNN_DEBUG_DIR, f)
            size = os.path.getsize(fpath)
            print(f"    {f} ({size} bytes)")
        if len(files) > 15:
            print(f"    ... and {len(files) - 15} more")

    # -------------------------------------------------------------------
    # Step 5: Validate results
    # -------------------------------------------------------------------
    print("\n[Step 5] Validating font matching results...")

    results = parse_ranking_results(diag)
    if not results:
        print("  WARNING: No ranking results parsed — cannot validate")
        print("  This may mean CNN did not trigger. Check screenshots.")
        screenshot("09_final")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        return 2

    winner = results[0]
    print(f"\n  Winner: {winner['family']} (visual={winner['visual']:.4f})")

    # Check: is the winner a serif font? (This test page is sans-serif)
    is_serif = winner["family"].lower().strip() in SERIF_FAMILIES
    if not is_serif:
        for sf in SERIF_FAMILIES:
            if sf in winner["family"].lower():
                is_serif = True
                break

    # Check: is the winner italic/oblique?
    is_italic = any(s in winner["family"].lower()
                    for s in ["italic", "oblique", "slanted"])

    # Check score spread
    if len(results) >= 10:
        spread = results[0]["visual"] - results[9]["visual"]
    else:
        spread = 0.0

    # Report
    print(f"\n  === VALIDATION RESULTS ===")
    print(f"  Winner family:     {winner['family']}")
    print(f"  Winner visual:     {winner['visual']:.4f}")
    print(f"  Is serif:          {'YES - FAIL' if is_serif else 'No - OK'}")
    print(f"  Is italic/oblique: {'YES - CONCERN' if is_italic else 'No - OK'}")
    print(f"  Score spread (1-10): {spread:.4f} {'(weak discrimination)' if spread < 0.05 else '(reasonable)'}")

    if len(results) >= 5:
        print(f"\n  Top 5:")
        for i, r in enumerate(results[:5]):
            flags = []
            fam_lower = r["family"].lower()
            if any(sf in fam_lower for sf in SERIF_FAMILIES):
                flags.append("SERIF")
            if any(s in fam_lower for s in ["italic", "oblique", "slanted"]):
                flags.append("ITALIC")
            flag_str = f"  [{', '.join(flags)}]" if flags else ""
            print(f"    {i+1}. {r['family']}  visual={r['visual']:.4f}{flag_str}")

    # Overall pass/fail
    errors = []
    warnings = []
    if is_serif:
        errors.append(f"Winner is serif font '{winner['family']}' on sans-serif page")
    if spread < 0.02:
        warnings.append(f"Score spread {spread:.4f} is narrow — model not discriminating well")
    if winner["visual"] < 0.2:
        warnings.append(f"Winner visual score {winner['visual']:.4f} is low — CNN has weak signal")

    print()
    for warn in warnings:
        print(f"  WARN: {warn}")
    if errors:
        for err in errors:
            print(f"  FAIL: {err}")
        result_code = 1
    else:
        print(f"  PASS: Sans-serif font selected, scores reasonable")
        if is_italic:
            print(f"  NOTE: Winner is italic/oblique — may indicate italic bias in model")
        result_code = 0

    # -------------------------------------------------------------------
    # Cleanup
    # -------------------------------------------------------------------
    screenshot("09_final")

    print("\n[Cleanup] Terminating PDFE...")
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    print(f"\nScreenshots saved to: {SCREENSHOT_DIR}")
    print(f"CNN debug output in: {CNN_DEBUG_DIR}")
    return result_code


if __name__ == "__main__":
    sys.exit(main())
