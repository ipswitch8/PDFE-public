# -*- coding: utf-8 -*-
"""Image-editing subsystem helpers for the PDFE UI test harness.

Provides high-level operations for the image management subsystem: inserting
images via file dialog, replacing images via the Image Tool selection flow,
applying CTM-based transforms via handle drags, cropping, rotating, and
stripping EXIF metadata from all document images.  All functions accept a
``PDFESession`` instance (from ``tests.ui_harness.core.launch``) as their
first argument and delegate low-level widget location, canvas interaction,
and screenshot capture to the shared ``core`` helpers.

Discovered objectNames (from src/ui/MainWindow.cpp image-action setup)
-----------------------------------------------------------------------
- imageToolAction        — QAction that activates the Image Tool (annotation toolbar)
                           accessibleName: "Image Tool"
- insertImageAction      — QAction for Image > Insert Image...
                           accessibleName: "Insert Image..."
- stripExifAction        — QAction for Image > Strip Image Metadata
                           accessibleName: "Strip Image Metadata"
- imageRemoveAction      — QAction for Image > Remove Image
                           accessibleName: "Remove Image"
- imageRotateAction      — QAction for Image > Rotate Image...
                           accessibleName: "Rotate Image..."  (wired to 90° CW)
- imageCropAction        — QAction for Image > Crop Image...
                           accessibleName: "Crop Image..."
- imageResizeAction      — QAction for Image > Resize Image...
                           accessibleName: "Resize Image..."
- imageMenu              — QMenu "Image" in the menu bar

ImageTool behaviour notes (from src/image/ImageTool.h / .cpp)
--------------------------------------------------------------
- ImageTool is activated by toggling imageToolAction.
- Clicking on the PDF canvas selects the image under the cursor; the tool
  emits imageSelected / imageDeselected signals.
- Resize handles appear at corners/edges; dragging them emits resizeRequested.
- A rotation handle appears above the top-centre edge; dragging it emits
  rotateRequested.
- The Delete key emits removeRequested; the menu actions delegate through
  onImageRemove / onImageRotate90 / onImageCrop / onImageResize in MainWindow.
- Because rotate/crop/resize in the menu use fixed deltas (90°, centre-80%
  crop, 50% resize), the test helpers that need precise control drive the
  canvas handle directly via PyAutoGUI drag.
- replace_image is not a native single-action flow; the helper removes the
  old image and inserts the new one at the same position using
  imageRemoveAction followed by insertImageAction.

File dialog triggered by Insert Image:
- QFileDialog::getOpenFileName, title "Insert Image"
  Filter: "Images (*.png *.jpg *.jpeg *.bmp *.gif *.tiff);;All Files (*)"
"""

import os
import time

import pyautogui
import pywinauto.timings

from tests.ui_harness.core.canvas import (
    canvas_rect,
    click_at,
    drag,
    wait_stable,
)
from tests.ui_harness.core.locate import (
    by_auto_id_or_name,
    by_name,
    invoke,
)
from tests.ui_harness.core.screenshot import capture_canvas

__all__ = [
    "insert_image",
    "replace_image",
    "transform_image",
    "crop_image",
    "rotate_image",
    "strip_exif",
]

# ---------------------------------------------------------------------------
# Internal constants — objectNames discovered from src/ui/MainWindow.cpp
# ---------------------------------------------------------------------------

_IMAGE_TOOL_ACTION_ID = "imageToolAction"
_IMAGE_TOOL_ACTION_NAME = "Image Tool"

_INSERT_IMAGE_ACTION_ID = "insertImageAction"
_INSERT_IMAGE_ACTION_NAME = "Insert Image..."

_STRIP_EXIF_ACTION_ID = "stripExifAction"
_STRIP_EXIF_ACTION_NAME = "Strip Image Metadata"

_IMAGE_REMOVE_ACTION_ID = "imageRemoveAction"
_IMAGE_REMOVE_ACTION_NAME = "Remove Image"

_IMAGE_ROTATE_ACTION_ID = "imageRotateAction"
_IMAGE_ROTATE_ACTION_NAME = "Rotate Image..."

_IMAGE_CROP_ACTION_ID = "imageCropAction"
_IMAGE_CROP_ACTION_NAME = "Crop Image..."

_IMAGE_RESIZE_ACTION_ID = "imageResizeAction"
_IMAGE_RESIZE_ACTION_NAME = "Resize Image..."

_CANVAS_ACCESSIBLE_NAME = "PDF Canvas"

# Image menu menu_select paths (fallback strategy).
_MENU_INSERT_IMAGE = "Image->Insert Image..."
_MENU_STRIP_EXIF = "Image->Strip Image Metadata"
_MENU_REMOVE_IMAGE = "Image->Remove Image"
_MENU_ROTATE_IMAGE = "Image->Rotate Image..."
_MENU_CROP_IMAGE = "Image->Crop Image..."
_MENU_RESIZE_IMAGE = "Image->Resize Image..."

# Timeouts.
_DIALOG_TIMEOUT = 15
_ACTION_SETTLE = 0.4
_TOOL_ACTIVATE_SETTLE = 0.3
_SELECTION_SETTLE = 0.3

# Strip-EXIF information dialog title pattern.
_STRIP_EXIF_INFO_TITLE_RE = r"Strip Image Metadata"


# ---------------------------------------------------------------------------
# Public helpers
# ---------------------------------------------------------------------------

def insert_image(session, path, x, y):
    """Invoke Insert Image, pick *path* from the file dialog, place at *(x, y)*.

    Activates the Image tool first (required so that PDFE is in image-editing
    mode), then triggers ``insertImageAction``.  After the file dialog closes,
    PDFE inserts the image at a default position (72, 72) on the current page —
    the caller-supplied *(x, y)* are screen coordinates clicked *after*
    insertion to move/confirm placement when applicable.  If the app is in
    insert-and-click mode (canvas click required to place) the click is sent;
    otherwise the click simply selects the inserted image via the Image Tool.

    Parameters
    ----------
    session : PDFESession
    path    : str   Absolute path to the image file to insert.
    x       : int   Screen X coordinate to click on the canvas after insertion.
    y       : int   Screen Y coordinate to click on the canvas after insertion.

    Raises
    ------
    TimeoutError
        If the Insert Image file dialog does not appear within
        ``_DIALOG_TIMEOUT`` seconds.
    RuntimeError
        If the insert action cannot be located or invoked.
    """
    path = os.path.abspath(path)

    # Ensure Image Tool is active so that image interactions are routed
    # through the correct tool dispatch path in PdfCanvas.
    _activate_image_tool(session)

    # Trigger the insert action.
    _invoke_action_or_menu(
        session,
        action_id=_INSERT_IMAGE_ACTION_ID,
        action_name=_INSERT_IMAGE_ACTION_NAME,
        menu_path=_MENU_INSERT_IMAGE,
    )

    # Wait for and handle the open-file dialog.
    _wait_for_dialog(session, title_re=r"Insert Image|Open", timeout=_DIALOG_TIMEOUT)
    pyautogui.hotkey("ctrl", "a")
    time.sleep(0.05)
    pyautogui.typewrite(path, interval=0.02)
    pyautogui.press("enter")

    # Give PDFE time to process the insertion and refresh the canvas.
    wait_stable(_ACTION_SETTLE)

    # Dismiss any informational dialog (e.g., "Image inserted successfully").
    _dismiss_dialog_if_present(session, title_re=r"Insert|Image|Success", timeout=2)

    # Click at the requested canvas position — either to confirm placement
    # in a click-to-place flow, or to select the newly inserted image.
    click_at(x, y)
    wait_stable(_SELECTION_SETTLE)


def replace_image(session, target_idx, path):
    """Select image at *target_idx* via the Image Tool and replace it with *path*.

    PDFE does not have a native "replace image" menu action.  This helper
    implements the flow as: activate Image Tool → click to select the image
    at *target_idx* (by 0-based canvas enumeration order, left-to-right /
    top-to-bottom within the current page) → invoke Remove Image → insert the
    replacement via Insert Image at the vacated position.

    Because image positions are determined at runtime, the helper locates the
    approximate canvas position of image *target_idx* by dividing the canvas
    into a grid based on index order (assuming images are arranged top-to-bottom
    within a single page).  For precise placement, prefer using canvas
    coordinates directly via ``insert_image``.

    Parameters
    ----------
    session    : PDFESession
    target_idx : int   0-based index of the image to replace on the current page.
    path       : str   Absolute path to the replacement image file.

    Raises
    ------
    TimeoutError
        If the file dialog does not appear within ``_DIALOG_TIMEOUT`` seconds.
    RuntimeError
        If the image tool or insert action cannot be invoked.
    """
    path = os.path.abspath(path)

    _activate_image_tool(session)

    # Click on the canvas at an estimated position for the target image.
    cx, cy = _estimate_image_position(session, target_idx)
    click_at(cx, cy)
    wait_stable(_SELECTION_SETTLE)

    # Capture the bounding box of the selected image for re-insertion.
    # The Remove action leaves the position vacant; we re-insert at the same spot.
    remove_x, remove_y = cx, cy

    # Remove the selected image.
    _invoke_action_or_menu(
        session,
        action_id=_IMAGE_REMOVE_ACTION_ID,
        action_name=_IMAGE_REMOVE_ACTION_NAME,
        menu_path=_MENU_REMOVE_IMAGE,
    )
    wait_stable(_ACTION_SETTLE)

    # Insert the replacement image.
    _invoke_action_or_menu(
        session,
        action_id=_INSERT_IMAGE_ACTION_ID,
        action_name=_INSERT_IMAGE_ACTION_NAME,
        menu_path=_MENU_INSERT_IMAGE,
    )

    _wait_for_dialog(session, title_re=r"Insert Image|Open", timeout=_DIALOG_TIMEOUT)
    pyautogui.hotkey("ctrl", "a")
    time.sleep(0.05)
    pyautogui.typewrite(path, interval=0.02)
    pyautogui.press("enter")

    wait_stable(_ACTION_SETTLE)
    _dismiss_dialog_if_present(session, title_re=r"Insert|Image|Success", timeout=2)

    # Click at the vacated position to confirm placement.
    click_at(remove_x, remove_y)
    wait_stable(_SELECTION_SETTLE)


def transform_image(session, rect, ctm):
    """Select the image overlapping *rect* and apply a CTM transform via drag.

    Selects the image by clicking the centre of *rect* (screen coordinates),
    then approximates the CTM transform as a resize drag on the bottom-right
    handle.  The handle is dragged from its current position to a new position
    computed from the scale components of *ctm* (ctm[0] for x-scale, ctm[3]
    for y-scale).

    *ctm* is a 6-element sequence ``[a, b, c, d, e, f]`` following the PDF
    CTM convention (a, d are x/y scales; b, c are shear; e, f are translation).
    Only the scale and translation components are honoured by this helper; shear
    is not applied via drag handles.

    Parameters
    ----------
    session : PDFESession
    rect    : tuple   (left, top, right, bottom) screen pixel bounding rect of
                      the image to select.
    ctm     : sequence of 6 floats   ``[a, b, c, d, e, f]``.

    Raises
    ------
    RuntimeError
        If the Image Tool cannot be activated.
    """
    left, top, right, bottom = rect
    cx = (left + right) // 2
    cy = (top + bottom) // 2
    w = right - left
    h = bottom - top

    _activate_image_tool(session)

    # Click to select the image.
    click_at(cx, cy)
    wait_stable(_SELECTION_SETTLE)

    # Decompose CTM: a=x-scale, d=y-scale, e=x-translation, f=y-translation.
    a, b, c, d, e, f = ctm

    # The bottom-right resize handle is at (right, bottom) in screen coords.
    # Drag it to (right * a + e, bottom * d + f) adjusted relative to the
    # canvas origin so the operation is meaningful at the tool level.
    # For simplicity we translate the CTM scale into a new corner position.
    new_right = int(left + w * a)
    new_bottom = int(top + h * d)

    # Apply translation offset (e, f) relative to the left/top anchor.
    new_right += int(e)
    new_bottom += int(f)

    # Drag the bottom-right handle.
    drag(right, bottom, new_right, new_bottom, button="left", duration=0.4)
    wait_stable(_ACTION_SETTLE)


def crop_image(session, target_idx, rect):
    """Select image at *target_idx* and apply the crop defined by *rect*.

    Activates the Image Tool, selects the image at *target_idx* by clicking
    its estimated canvas position, then invokes ``imageCropAction``.  The
    PDFE menu action crops to the centre-80% of the bounding box.  To apply
    a *specific* crop rect, this helper instead drags the crop handles after
    invoking the action, using *rect* as the target crop region in screen
    coordinates.

    Parameters
    ----------
    session    : PDFESession
    target_idx : int   0-based image index on the current page.
    rect       : tuple (left, top, right, bottom) in screen pixels for the
                 desired crop region.

    Raises
    ------
    RuntimeError
        If the image cannot be selected or the crop action invoked.
    """
    _activate_image_tool(session)

    cx, cy = _estimate_image_position(session, target_idx)
    click_at(cx, cy)
    wait_stable(_SELECTION_SETTLE)

    # Invoke the crop action (applies centre-80% crop via MainWindow).
    _invoke_action_or_menu(
        session,
        action_id=_IMAGE_CROP_ACTION_ID,
        action_name=_IMAGE_CROP_ACTION_NAME,
        menu_path=_MENU_CROP_IMAGE,
    )
    wait_stable(_ACTION_SETTLE)

    # If a specific crop rect was supplied, drag the crop handles to
    # the requested corners.  The crop overlay, when active, presents
    # corner and edge handles analogous to resize handles.
    left, top, right, bottom = rect
    new_cx = (left + right) // 2
    new_cy = (top + bottom) // 2

    # Re-select the image (the crop action may have deselected it).
    click_at(cx, cy)
    wait_stable(_SELECTION_SETTLE)

    # Drag the bottom-right corner handle to the requested bottom-right.
    drag(cx + 20, cy + 20, right, bottom, button="left", duration=0.35)
    wait_stable(_ACTION_SETTLE)

    # Drag the top-left corner handle to the requested top-left.
    drag(cx - 20, cy - 20, left, top, button="left", duration=0.35)
    wait_stable(_ACTION_SETTLE)


def rotate_image(session, target_idx, degrees):
    """Select image at *target_idx* and rotate it by *degrees*.

    Activates the Image Tool and selects the image by clicking its estimated
    canvas position.  For 90° and 180° increments the corresponding menu
    action is used (imageRotateAction wires to 90° CW in MainWindow; a second
    invocation achieves 180°).  For arbitrary angles, the rotation handle
    (above the top-centre of the bounding box) is dragged by the angle
    converted to a tangential screen offset.

    Parameters
    ----------
    session    : PDFESession
    target_idx : int   0-based image index on the current page.
    degrees    : float Rotation in degrees (positive = clockwise).

    Raises
    ------
    RuntimeError
        If the image cannot be selected or the rotate action invoked.
    """
    import math

    _activate_image_tool(session)

    cx, cy = _estimate_image_position(session, target_idx)
    click_at(cx, cy)
    wait_stable(_SELECTION_SETTLE)

    if degrees in (90, 180, 270):
        # Use the menu action for clean 90°/180° rotations.
        rotations = int(degrees) // 90
        for _ in range(rotations):
            _invoke_action_or_menu(
                session,
                action_id=_IMAGE_ROTATE_ACTION_ID,
                action_name=_IMAGE_ROTATE_ACTION_NAME,
                menu_path=_MENU_ROTATE_IMAGE,
            )
            wait_stable(_ACTION_SETTLE)
            # Re-select after each rotation so subsequent invocations work.
            if rotations > 1:
                click_at(cx, cy)
                wait_stable(_SELECTION_SETTLE)
    else:
        # Arbitrary angle: drag the rotation handle (placed ~30 px above
        # the top-centre of the estimated image bounding box).
        # Estimate handle position: 30 px above the top edge of the image.
        handle_x = cx
        handle_y = cy - 50  # Approximate handle distance above centre.

        # Compute drag target: rotate the handle position by the requested
        # angle around the image centre.
        rad = math.radians(degrees)
        rel_x = handle_x - cx
        rel_y = handle_y - cy
        new_rel_x = rel_x * math.cos(rad) - rel_y * math.sin(rad)
        new_rel_y = rel_x * math.sin(rad) + rel_y * math.cos(rad)
        target_x = int(cx + new_rel_x)
        target_y = int(cy + new_rel_y)

        drag(handle_x, handle_y, target_x, target_y, button="left", duration=0.4)
        wait_stable(_ACTION_SETTLE)


def strip_exif(session, target_idx):
    """Invoke Strip Image Metadata against the whole document.

    PDFE's stripExifAction strips metadata from *all* images in the open
    document (not just the one at *target_idx*).  *target_idx* is accepted for
    API consistency with the acceptance criteria but is not used to narrow
    scope; PDFE does not support per-image EXIF stripping via the UI.

    After invocation an informational QMessageBox appears indicating how many
    images were processed.  This helper dismisses that dialog automatically.

    Parameters
    ----------
    session    : PDFESession
    target_idx : int   Included for API symmetry; not used by PDFE's strip action.

    Raises
    ------
    RuntimeError
        If the strip action cannot be located or invoked.
    """
    _invoke_action_or_menu(
        session,
        action_id=_STRIP_EXIF_ACTION_ID,
        action_name=_STRIP_EXIF_ACTION_NAME,
        menu_path=_MENU_STRIP_EXIF,
    )

    # PDFE shows a QMessageBox::information with the count of stripped images.
    # Dismiss it with Enter so the test can proceed.
    _dismiss_dialog_if_present(
        session,
        title_re=_STRIP_EXIF_INFO_TITLE_RE,
        timeout=5,
    )

    wait_stable(_ACTION_SETTLE)


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _activate_image_tool(session):
    """Toggle imageToolAction to activate the Image Tool in PdfCanvas.

    Locates the action by objectName ``imageToolAction`` (UIA AutomationId),
    falling back to accessibleName ``"Image Tool"``.  If the action is already
    checked (tool already active) the invocation is skipped.

    Parameters
    ----------
    session : PDFESession

    Raises
    ------
    RuntimeError
        If the image tool action cannot be located or invoked.
    """
    window = session.window

    try:
        ctrl = by_auto_id_or_name(
            window, _IMAGE_TOOL_ACTION_ID, _IMAGE_TOOL_ACTION_NAME, timeout=10
        )
        # Attempt to check whether the action is already checked before clicking.
        try:
            if ctrl.get_toggle_state() == 1:
                return  # Already active.
        except Exception:
            pass  # get_toggle_state not supported — just invoke it.
        invoke(ctrl)
        wait_stable(_TOOL_ACTIVATE_SETTLE)
        return
    except Exception:
        pass

    # Fallback: use the annotation toolbar menu item if UIA direct lookup fails.
    try:
        window.set_focus()
        # imageToolAction lives in the annotation toolbar; no standard menu path.
        # Attempt keyboard shortcut — none assigned by default, so this falls through.
        raise RuntimeError("UIA lookup failed")
    except Exception as exc:
        raise RuntimeError(
            f"Could not activate Image Tool "
            f"(auto_id={_IMAGE_TOOL_ACTION_ID!r}, "
            f"name={_IMAGE_TOOL_ACTION_NAME!r}): {exc}"
        ) from exc


def _estimate_image_position(session, idx):
    """Estimate (screen_x, screen_y) for the *idx*-th image on the current page.

    Uses the canvas bounding rect from ``canvas_rect()`` and distributes
    positions evenly in a top-to-bottom column.  This is an approximation;
    for PDFs with known image layouts, pass explicit screen coordinates to the
    public helpers instead.

    Parameters
    ----------
    session : PDFESession
    idx     : int   0-based image index.

    Returns
    -------
    (int, int)  absolute screen coordinates

    Raises
    ------
    RuntimeError
        If the canvas rect cannot be obtained.
    """
    try:
        r = canvas_rect(session.app)
        canvas_w = r.right - r.left
        canvas_h = r.bottom - r.top

        # Divide the canvas into a 3-column grid and assign rows by index.
        cols = 3
        cell_w = canvas_w // cols
        cell_h = max(canvas_h // max(1, idx + 1), 80)  # Adaptive row height.

        col = idx % cols
        row = idx // cols

        cx = r.left + col * cell_w + cell_w // 2
        cy = r.top + row * cell_h + cell_h // 2

        return (cx, cy)
    except Exception as exc:
        raise RuntimeError(
            f"_estimate_image_position: cannot compute position for image {idx}: {exc}"
        ) from exc


def _invoke_action_or_menu(session, action_id, action_name, menu_path):
    """Invoke a QAction via UIA auto_id lookup, falling back to menu navigation.

    Parameters
    ----------
    session     : PDFESession
    action_id   : str   objectName of the QAction (UIA AutomationId).
    action_name : str   accessibleName fallback.
    menu_path   : str   pywinauto menu_select path (e.g. "Image->Insert Image...").

    Raises
    ------
    RuntimeError
        If both strategies fail.
    """
    window = session.window

    # Strategy 1: direct UIA node lookup.
    try:
        ctrl = by_auto_id_or_name(window, action_id, action_name, timeout=5)
        invoke(ctrl)
        return
    except Exception:
        pass

    # Strategy 2: menu navigation.
    try:
        window.set_focus()
        # GAP-A14: UIA menu walk fallback, retained only for interactive/local
        # hosts where menu_select is reliable. On Server-2019 CI it can deadlock —
        # prefer invoke_action(window, action_id) which fires the QAction via the
        # testToolActivatePipe seam instead of enumerating the UIA menu tree.
        window.menu_select(menu_path)
        return
    except Exception as exc:
        raise RuntimeError(
            f"Could not invoke action "
            f"(auto_id={action_id!r}, name={action_name!r}, menu={menu_path!r}): {exc}"
        ) from exc


def _wait_for_dialog(session, title_re, timeout=_DIALOG_TIMEOUT):
    """Block until a top-level dialog whose title matches *title_re* appears.

    Parameters
    ----------
    session  : PDFESession
    title_re : str   Regular expression matched against the dialog title.
    timeout  : int   Seconds to wait.

    Returns
    -------
    pywinauto WindowSpecification for the dialog.

    Raises
    ------
    TimeoutError
        If no matching dialog appears within *timeout* seconds.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            dlg = session.app.window(title_re=title_re)
            dlg.wait("visible", timeout=1)
            return dlg
        except Exception:
            time.sleep(0.3)

    raise TimeoutError(
        f"Dialog matching title_re={title_re!r} did not appear within {timeout}s."
    )


def _dismiss_dialog_if_present(session, title_re, timeout=3):
    """Accept a dialog (press Enter) if it appears within *timeout* seconds.

    Used to dismiss informational QMessageBox dialogs without raising if no
    dialog appears.

    Parameters
    ----------
    session  : PDFESession
    title_re : str   Regex to match against dialog title.
    timeout  : int   Seconds to wait for the dialog.
    """
    try:
        _wait_for_dialog(session, title_re=title_re, timeout=timeout)
        pyautogui.press("enter")
        wait_stable(0.2)
    except TimeoutError:
        pass  # No dialog appeared — that is fine.
