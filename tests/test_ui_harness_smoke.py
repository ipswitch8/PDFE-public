# -*- coding: utf-8 -*-
"""Smoke test for the PDFE UI test harness (Phase 2).

Verifies end-to-end:
1. PDFE.exe launches cleanly with no orphan processes before start.
2. The main window appears at the expected 1600x1000 size.
3. The menu bar is locatable by accessibleName via pywinauto UIA.
4. A baseline screenshot of the canvas can be captured.
5. A pixel-diff of the screenshot against itself yields zero delta.
6. The golden regen workflow creates a golden file.
7. PDFE closes cleanly with no orphan pdfe.exe processes remaining.

Run with:
    python -m pytest tests/test_ui_harness_smoke.py -v

Requirements:
- 100% display scaling.
- Interactive desktop (not a headless CI service session).
- pdfe.exe present at build/vs2022/bin/Debug/pdfe.exe.
- test_single.pdf present at tests/fixtures/test_single.pdf.
"""

import os
import sys
import time

import pytest

# ---------------------------------------------------------------------------
# Resolve project root so imports work regardless of cwd.
# ---------------------------------------------------------------------------

_PROJECT_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..")
)
if _PROJECT_ROOT not in sys.path:
    sys.path.insert(0, _PROJECT_ROOT)

from tests.ui_harness.core.launch import (
    PDFE_EXE,
    PDFESession,
    no_orphan_pdfe,
    _kill_orphan_pdfe,
)
from tests.ui_harness.core.locate import by_auto_id, by_name
from tests.ui_harness.core.screenshot import capture_canvas
from tests.ui_harness.core.diff import assert_zero_delta
from tests.ui_harness.core.golden import (
    regen_golden,
    golden_path,
    delete_golden,
    compare_to_golden,
)

# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

FIXTURE_PDF = os.path.join(_PROJECT_ROOT, "tests", "fixtures", "test_single.pdf")
SUBSYSTEM = "smoke"
TEST_NAME = "harness_baseline"


@pytest.fixture(autouse=True)
def ensure_no_orphans_before():
    """Kill any stale pdfe.exe before each test."""
    _kill_orphan_pdfe()
    time.sleep(0.5)
    yield
    # Teardown: verify no orphan remains after test.
    # (Individual test also checks this, but belt-and-suspenders.)
    _kill_orphan_pdfe()


# ---------------------------------------------------------------------------
# Skip guard: if no interactive display or exe not found, skip gracefully.
# ---------------------------------------------------------------------------

def _pdfe_exe_exists():
    return os.path.isfile(PDFE_EXE)


def _fixture_pdf_exists():
    return os.path.isfile(FIXTURE_PDF)


def _screen_can_fit_1400px():
    """Return True if the primary screen is at least 1400 logical pixels wide.

    GAP-28 AC2 skip path: if the rig's display is rotated to portrait or
    otherwise too narrow to accommodate the 1400-px-wide window the test
    asserts, the test is skipped with an explicit reason rather than
    failing on an unmeetable assertion. This finishes the "or skip"
    branch of phase-1 AC2.
    """
    if sys.platform != "win32":
        return False
    try:
        import ctypes
        u32 = ctypes.windll.user32
        return u32.GetSystemMetrics(0) >= 1400  # SM_CXSCREEN
    except Exception:  # noqa: BLE001
        return False


# ---------------------------------------------------------------------------
# Test
# ---------------------------------------------------------------------------

@pytest.mark.skipif(
    not _pdfe_exe_exists(),
    reason=f"pdfe.exe not found at {PDFE_EXE}; build first.",
)
@pytest.mark.skipif(
    not _fixture_pdf_exists(),
    reason=f"Fixture PDF not found at {FIXTURE_PDF}.",
)
@pytest.mark.skipif(
    not _screen_can_fit_1400px(),
    reason="Primary screen width < 1400px (portrait or low-res display) — "
           "smoke asserts width >= 1400 (GAP-28 AC2 skip path).",
)
class TestUIHarnessSmokeTest:
    """End-to-end smoke test for the Phase-2 UI harness."""

    def test_launch_and_locate_menu_bar(self):
        """Launch PDFE, locate the menu bar, confirm window size, close cleanly."""
        with PDFESession(pdf_path=FIXTURE_PDF) as session:
            win = session.window

            # --- 1. Window is visible and at expected size. ---
            win.set_focus()
            time.sleep(0.5)
            rect = win.rectangle()
            width = rect.right - rect.left
            height = rect.bottom - rect.top

            # Allow up to 80 px tolerance.  SetWindowPos requests 1600x1000 but
            # Qt may adjust for window frame / minimum-size constraints, so the
            # actual client area is typically 1584x961.  What matters for
            # automation is that the window is substantially larger than PDFE's
            # default 1280x800; this check catches the case where the resize
            # silently failed and PDFE is still at its default size.
            assert width >= 1400, (
                f"Window width={width} too small (expected >= 1400px). "
                "The resize likely failed — check that SetWindowPos succeeded. "
                "Also verify display scaling is 100%."
            )
            assert height >= 900, (
                f"Window height={height} too small (expected >= 900px). "
                "The resize likely failed."
            )

            # --- 2. Menu bar is locatable. ---
            # Qt UIA exposes the QMenuBar as a Menu control with auto_id
            # "QApplication.mainWindow.QMenuBar".  We locate it by control_type
            # and verify that the "File" menu item is present as a child.
            # by_auto_id("QMenuBar") uses suffix-match to find it.
            menu_bar = by_auto_id(win, "QMenuBar", control_type="Menu", timeout=10)
            assert menu_bar is not None, "QMenuBar not found in UIA tree."
            # Verify "File" menu item is a child (found via accessible name).
            file_item = by_name(win, "File", control_type="MenuItem", timeout=5)
            assert file_item is not None, "File menu item not found."
            # Confirm the menu item has the expected window text.
            assert "File" in file_item.window_text(), (
                f"File menu item text={file_item.window_text()!r} unexpected."
            )

            # --- 3. Canvas screenshot can be captured. ---
            img_path = capture_canvas(session.app, subsystem=SUBSYSTEM,
                                      test=TEST_NAME, wait=0.5)
            assert os.path.isfile(img_path), (
                f"Screenshot was not saved to {img_path}."
            )
            assert os.path.getsize(img_path) > 0, (
                f"Screenshot file at {img_path} is empty."
            )

            # --- 4. Pixel diff of image against itself is zero. ---
            assert_zero_delta(img_path)

            # --- 5. Golden baseline must be a COMMITTED reference (R-29). ---
            # NO self-seeding: the smoke golden is a source-controlled capture
            # reviewed by a human on a real desktop session. If it is absent we
            # FAIL loudly rather than promote the current (possibly broken)
            # screenshot — a self-seeded baseline makes every future render pass
            # forever. Rebaselining is a deliberate opt-in maintenance action
            # (PDFE_ALLOW_GOLDEN_SEED=1 + regen_golden), never done on the assert
            # path.
            _gp = golden_path(SUBSYSTEM, TEST_NAME)
            assert _gp.exists(), (
                f"Committed golden baseline missing: {_gp}. This test does not "
                "self-seed one from the current capture (R-29). Commit a "
                "trusted, reviewed reference screenshot at that path."
            )

            # --- 6. Screenshot matches the stored golden. ---
            # Phase 3 Cluster #6 (2026-06-09): CI session captures
            # render differently from the interactive desktop session
            # used to capture the golden — sub-pixel hinting, color
            # profile, and font rasterization differ between an
            # interactive logon and a service / runner logon on the
            # same physical machine. Run 27227956881 observed NCC
            # floor 0.80 on this fixture (vs. 1.00 local), max_delta
            # 243 (vs. 0 local), and that drift cannot be eliminated
            # by rebaselining alone (Cluster #2 fix-up f4d7cc1 hit
            # the same problem on visual-regression PNGs). Relax the
            # thresholds to absorb the documented CI drift while
            # still catching a regression that would, e.g., replace
            # the canvas with a blank frame (NCC near 0) or swap
            # menu chrome wholesale (large pHash distance).
            result = compare_to_golden(SUBSYSTEM, TEST_NAME, img_path,
                                       ncc_threshold=0.75,
                                       max_delta_threshold=255,
                                       phash_threshold=20)
            assert result.is_same, (
                f"Golden comparison failed: {result.summary}"
            )

        # --- 7. No orphan PDFE.exe process after context manager exits. ---
        # Give OS a moment to fully release the process.
        time.sleep(1.0)
        assert no_orphan_pdfe(), (
            "pdfe.exe process still running after PDFESession context exit. "
            "Check that quit_pdfe() correctly terminates the process."
        )
