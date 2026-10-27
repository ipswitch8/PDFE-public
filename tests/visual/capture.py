# -*- coding: utf-8 -*-
"""Window and screen capture helpers + system DPI detection.

Two capture paths are exposed:

1. ``capture_hwnd(hwnd, region=None)`` — uses Win32 BitBlt over the
   window DC.  PrintWindow with the PW_RENDERFULLCONTENT (0x02) flag is
   tried first for windows that draw via OpenGL/D3D where BitBlt would
   otherwise yield a black rectangle.  Falls back to BitBlt on the
   foreground DC when PrintWindow returns nothing.

2. ``capture_screen_region(x, y, w, h)`` — uses PIL.ImageGrab over the
   composited desktop.  Used when an HWND-based capture cannot isolate
   the target (e.g. a popup that is not a top-level window) or when
   PrintWindow's renderfullcontent path fails on the canvas widget.

DPI detection
-------------
``detect_system_dpi()`` returns the primary-monitor effective DPI,
preferring the per-window API on Windows 10+ and falling back to
``GetDeviceCaps`` on older systems.  Used by the demo tests to skip
when the host DPI is not in the supported set {96, 120, 144, 192}
rather than producing false-failing pixel diffs.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
from typing import Optional, Tuple

from PIL import Image, ImageGrab


# ---------------------------------------------------------------------------
# Win32 imports (resolved lazily so the module imports cleanly on non-Windows
# CI even though the tests will skip there).
# ---------------------------------------------------------------------------

def _win32_modules():
    import win32gui  # type: ignore
    import win32ui   # type: ignore
    import win32con  # type: ignore
    return win32gui, win32ui, win32con


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def capture_hwnd(
    hwnd: int,
    region: Optional[Tuple[int, int, int, int]] = None,
) -> Image.Image:
    """Capture a window (or a region within it) by HWND.

    Parameters
    ----------
    hwnd   : int    Win32 window handle.
    region : (x, y, w, h) or None
        If provided, returns just that sub-rectangle of the window's
        client area.  Otherwise returns the entire client area.

    Returns
    -------
    PIL.Image.Image (RGB)
    """
    win32gui, win32ui, win32con = _win32_modules()

    # Client-area dimensions
    left, top, right, bottom = win32gui.GetClientRect(hwnd)
    full_w = right - left
    full_h = bottom - top
    if full_w <= 0 or full_h <= 0:
        raise ValueError(
            f"capture_hwnd: hwnd={hwnd} has empty client rect "
            f"({full_w}x{full_h})."
        )

    if region is None:
        rx, ry, rw, rh = 0, 0, full_w, full_h
    else:
        rx, ry, rw, rh = region
        if rw <= 0 or rh <= 0:
            raise ValueError(f"capture_hwnd: bad region {region}")

    # Try PrintWindow with PW_RENDERFULLCONTENT (Windows 8.1+) — required for
    # windows that draw via OpenGL/D3D so we don't get a black rectangle.
    try:
        img = _print_window_to_image(hwnd, full_w, full_h)
        if img is not None:
            if region is None:
                return img
            return img.crop((rx, ry, rx + rw, ry + rh))
    except Exception:  # noqa: BLE001
        pass

    # Fallback: BitBlt the screen DC at the window's screen position.  This
    # works when the window is on the foreground but cannot capture occluded
    # regions.
    win32gui_2, _win32ui_2, _win32con_2 = _win32_modules()
    sx, sy, _, _ = win32gui_2.GetWindowRect(hwnd)
    cx, cy = win32gui_2.ClientToScreen(hwnd, (rx, ry))
    return capture_screen_region(cx, cy, rw, rh)


def capture_screen_region(x: int, y: int, w: int, h: int) -> Image.Image:
    """Capture an absolute-screen region via PIL.ImageGrab (RGB)."""
    if w <= 0 or h <= 0:
        raise ValueError(f"capture_screen_region: bad size {w}x{h}")
    img = ImageGrab.grab(bbox=(x, y, x + w, y + h), all_screens=True)
    return img.convert("RGB")


def detect_system_dpi() -> int:
    """Return the primary-monitor effective DPI.

    Tries (in order):
        1. user32.GetDpiForSystem  (Win10 1607+)
        2. shcore.GetScaleFactorForDevice  (Win8.1+)
        3. GDI GetDeviceCaps(LOGPIXELSX) on the desktop DC

    Returns 96 (the canonical 100% DPI) if all probes fail.
    """
    # 1. GetDpiForSystem
    try:
        get_dpi_for_system = ctypes.windll.user32.GetDpiForSystem
        get_dpi_for_system.restype = wt.UINT
        dpi = int(get_dpi_for_system())
        if dpi > 0:
            return dpi
    except (AttributeError, OSError):
        pass

    # 2. GetScaleFactorForDevice (returns scale% — 100, 125, 150, ...)
    try:
        get_scale = ctypes.windll.shcore.GetScaleFactorForDevice
        get_scale.argtypes = [ctypes.c_int]
        get_scale.restype = ctypes.c_int
        # DEVICE_PRIMARY = 0
        scale_pct = int(get_scale(0))
        if scale_pct > 0:
            return int(round(scale_pct * 96.0 / 100.0))
    except (AttributeError, OSError):
        pass

    # 3. GDI fallback
    try:
        user32 = ctypes.windll.user32
        gdi32 = ctypes.windll.gdi32
        hdc = user32.GetDC(0)
        if hdc:
            try:
                LOGPIXELSX = 88
                dpi = int(gdi32.GetDeviceCaps(hdc, LOGPIXELSX))
                if dpi > 0:
                    return dpi
            finally:
                user32.ReleaseDC(0, hdc)
    except (AttributeError, OSError):
        pass

    return 96


# ---------------------------------------------------------------------------
# Internals
# ---------------------------------------------------------------------------

def _print_window_to_image(hwnd: int, w: int, h: int) -> Optional[Image.Image]:
    """Use Win32 PrintWindow + PW_RENDERFULLCONTENT to grab the window
    surface, including OpenGL/D3D content.  Returns None on failure.
    """
    win32gui, win32ui, win32con = _win32_modules()

    hwnd_dc = win32gui.GetWindowDC(hwnd)
    if not hwnd_dc:
        return None
    src_dc = None
    mem_dc = None
    bmp = None
    try:
        src_dc = win32ui.CreateDCFromHandle(hwnd_dc)
        mem_dc = src_dc.CreateCompatibleDC()
        bmp = win32ui.CreateBitmap()
        bmp.CreateCompatibleBitmap(src_dc, w, h)
        mem_dc.SelectObject(bmp)

        PW_CLIENTONLY = 0x00000001
        PW_RENDERFULLCONTENT = 0x00000002

        # PW_CLIENTONLY IS REQUIRED, NOT OPTIONAL (GitHub #279).
        #
        # The bitmap above is allocated at CLIENT size (GetClientRect), but
        # PrintWindow without PW_CLIENTONLY renders the WHOLE WINDOW starting at
        # the WINDOW origin -- non-client frame included. The result is the
        # window's top-left corner carrying client dimensions: every pixel
        # displaced down-right by the non-client offset, and the bottom-right of
        # the client area cut off. Callers then crop it with regions expressed in
        # genuine client coordinates (canvas_region_client_px reads the
        # testCanvasRect seam, which maps inside the application), so every crop
        # landed that offset too high.
        #
        # Measured on this rig: ClientToScreen(0,0) minus GetWindowRect gives
        # dx=8 dy=31, and the toolbar's chrome began at row 31 of the capture
        # under the old flags and at row 0 under these -- the displacement and
        # the independently-measured offset agree exactly. In the canvas
        # sub-rect that was 30 rows of toolbar before and 0 after.
        #
        # The visible symptom was ten phase5_* canvas oracles failing at SSIM
        # 0.93-0.95: the page crop's top 20 rows were toolbar, so the oracles
        # were asserting which buttons the toolbar's overflow happened to be
        # showing. The page pixels underneath were byte-identical throughout.
        #
        # PW=0 is kept as the second chance for the same reason as before (some
        # drivers refuse PW_RENDERFULLCONTENT), but it keeps PW_CLIENTONLY --
        # dropping it would silently restore the misalignment on exactly the
        # machines where the first call failed, which is the worst case to debug.
        result = ctypes.windll.user32.PrintWindow(
            hwnd, mem_dc.GetSafeHdc(), PW_CLIENTONLY | PW_RENDERFULLCONTENT
        )
        if not result:
            result = ctypes.windll.user32.PrintWindow(
                hwnd, mem_dc.GetSafeHdc(), PW_CLIENTONLY
            )
            if not result:
                return None

        bits = bmp.GetBitmapBits(True)
        img = Image.frombuffer(
            "RGB", (w, h), bits, "raw", "BGRX", 0, 1
        )

        # Detect a fully black image (PrintWindow silent failure mode for
        # composited windows) and force the screen-region fallback.
        if _is_uniform_black(img):
            return None
        return img.copy()
    finally:
        try:
            if bmp is not None:
                win32gui.DeleteObject(bmp.GetHandle())
        except Exception:  # noqa: BLE001
            pass
        try:
            if mem_dc is not None:
                mem_dc.DeleteDC()
        except Exception:  # noqa: BLE001
            pass
        try:
            if src_dc is not None:
                src_dc.DeleteDC()
        except Exception:  # noqa: BLE001
            pass
        try:
            win32gui.ReleaseDC(hwnd, hwnd_dc)
        except Exception:  # noqa: BLE001
            pass


def _is_uniform_black(img: Image.Image, sample_step: int = 31) -> bool:
    """Return True if the image is (essentially) all black.

    Sampled rather than full-pixel for speed.
    """
    w, h = img.size
    if w == 0 or h == 0:
        return True
    px = img.load()
    seen_non_black = 0
    for y in range(0, h, max(1, h // sample_step)):
        for x in range(0, w, max(1, w // sample_step)):
            r, g, b = px[x, y][:3]
            if r > 8 or g > 8 or b > 8:
                seen_non_black += 1
                if seen_non_black >= 4:
                    return False
    return True
