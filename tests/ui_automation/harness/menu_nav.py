# -*- coding: utf-8 -*-
"""Qt menu navigation helpers.

Qt's UIA backend assigns every ``QAction`` the same auto_id
(``QApplication.mainWindow.QMenuBar.QAction``), so menu items cannot
be located by auto_id. They must be navigated by their displayed
title, which remains unique within a single menu level.

These helpers wrap the two common cases:

1. :func:`open_top_menu` — click a top-level menu to expand it.
2. :func:`click_menu_path` — navigate a full menu path like
   ``"File -> Open"``, ``"Help -> About PDFE"``.
"""
from __future__ import annotations

import time
from typing import List


def open_top_menu(main_window, title: str, timeout: float = 8.0) -> None:
    """Click a top-level menu item to open its dropdown.

    Pipeline f3c91a2e phase-3 Cluster #1B: bumped wait timeout from
    5.0s to 8.0s to match the leaf-popup timeout — under sustained
    test load on the RDP runner the top-level menubar's UIA visibility
    transition can lag a few seconds behind the main window's appearance.
    Also explicitly set_focus() on the main window before clicking; if
    the window has lost foreground focus (e.g. another test's dialog
    just dismissed), the absolute-coords click_input() can land on a
    stale HWND and silently no-op.

    :param main_window: The pywinauto main window wrapper.
    :param title: The menu title text, e.g. ``"File"``.
    :param timeout: How long to wait for the top-level menu item to
                    become visible before clicking.
    """
    top = main_window.child_window(title=title, control_type="MenuItem")
    top.wait("visible", timeout=timeout)
    # Ensure the main window has foreground focus so the absolute
    # mouse click lands on the menubar HWND rather than the previous
    # foreground window. Best-effort — if set_focus fails, click_input
    # will still try.
    try:
        main_window.set_focus()
        time.sleep(0.1)
    except Exception:  # noqa: BLE001
        pass
    top.click_input()
    # Qt menu popup animation is synchronous on Windows but give the
    # event loop a few ms to paint before callers look for children.
    time.sleep(0.4)


def _find_menu_item_in_popup(main_window, title: str, timeout: float = 3.0):
    """Locate a MenuItem with ``title`` in the active Qt popup window.

    Qt 6 spawns menu popups as top-level Desktop windows (control_type
    ``Menu``) rather than as descendants of the parent window. The
    ``main_window.child_window(...)`` lookup misses them entirely. This
    helper walks ``Desktop().windows()``, filters by the same process,
    finds the live ``Menu`` popup, and returns the matching child item.

    Falls back to the original ``main_window`` descendant search for Qt
    builds where popups are parented to the main window.

    :raises TimeoutError: If no matching item appears within ``timeout``.
    """
    from pywinauto import Desktop  # local import — pywinauto is optional

    pid = main_window.process_id()
    deadline = time.time() + timeout
    last_err: Exception | None = None
    while time.time() < deadline:
        # Pipeline f7c2a4e8 phase-6 (D1 / P-GAP-88): strip mnemonic
        # ampersands when comparing window_text. Qt QActions defined
        # with tr("&New") may expose UIA Name as either "New" (Qt
        # stripped the mnemonic) or "&New" (verbatim). The matcher
        # must accept both. Strip "&" from both sides.
        def _norm(s):
            return (s or "").replace("&", "")
        norm_target = _norm(title)

        # Try the desktop-popup path first (Qt 6 default).
        for w in Desktop(backend="uia").windows():
            try:
                if w.process_id() != pid:
                    continue
                if w.element_info.control_type != "Menu":
                    continue
                for item in w.descendants(control_type="MenuItem"):
                    try:
                        if _norm(item.window_text()) == norm_target:
                            return item
                    except Exception:  # noqa: BLE001
                        continue
            except Exception as e:  # noqa: BLE001
                last_err = e
                continue

        # Fallback: child_window under main_window for older Qt builds.
        try:
            item = main_window.child_window(
                title=title, control_type="MenuItem"
            )
            if item.exists(timeout=0.2):
                return item
        except Exception as e:  # noqa: BLE001
            last_err = e

        time.sleep(0.15)

    raise TimeoutError(
        f"Menu item {title!r} not found in any popup within {timeout}s"
        + (f" (last error: {last_err})" if last_err else "")
    )


def click_menu_path(main_window, path: str, leaf_timeout: float = 8.0) -> None:
    """Navigate a menu path like ``"File -> Open"`` and click the leaf.

    Splits on ``" -> "`` and walks each level by clicking its title.
    Works around Qt's shared QAction auto_ids and Qt 6's Desktop-level
    menu popups.

    Pipeline f3c91a2e phase-3 Cluster #1B: bumped default leaf_timeout
    from 3.0s to 8.0s. On the RDP CI runner the Qt menu-popup HWND can
    take 4-6s to materialize after a top-level click under sustained
    test load. The 3s timeout was producing spurious "Menu item 'Open...'
    not found in any popup within 3.0s" failures even when the popup
    was about to appear. The first top-level click is also retried up
    to twice if the popup is not visible after the first attempt; on
    the RDP host the initial click occasionally lands while the
    foreground HWND is still resolving and Qt swallows the press.

    :param main_window: The pywinauto main window wrapper.
    :param path: Menu path with ``" -> "`` separators.
    :param leaf_timeout: Per-leaf popup-enumeration timeout in seconds.
    """
    parts = [p.strip() for p in path.split("->")]
    if not parts:
        raise ValueError(f"Empty menu path: {path!r}")

    # Retry the top-level open up to 3 times; if the first click on the
    # menubar fails to spawn a popup (no popup leaves materialize), it
    # is almost always because the click landed before the window had
    # foreground focus. Re-clicking with another set_focus() typically
    # succeeds.
    last_err: Exception | None = None
    leaf = parts[1] if len(parts) >= 2 else None
    for attempt in range(3):
        try:
            open_top_menu(main_window, parts[0])
            if leaf is None:
                return
            # Use the first leaf to confirm the popup materialized — a
            # short timeout on the probe is fine because the slow path is
            # the popup spawn, not the enumeration.
            probe_timeout = leaf_timeout if attempt == 2 else 3.0
            item = _find_menu_item_in_popup(main_window, leaf,
                                            timeout=probe_timeout)
            item.click_input()
            time.sleep(0.2)
            # Walk any remaining levels (uncommon — most menus are 2 deep).
            for next_leaf in parts[2:]:
                nxt = _find_menu_item_in_popup(main_window, next_leaf,
                                               timeout=leaf_timeout)
                nxt.click_input()
                time.sleep(0.2)
            return
        except TimeoutError as e:
            last_err = e
            # Send Escape to dismiss any lingering popup so the next
            # attempt is from a clean state.
            try:
                main_window.type_keys("{ESC}")
                time.sleep(0.3)
            except Exception:  # noqa: BLE001
                pass
            continue

    # All retries exhausted — surface the original timeout for clarity.
    raise last_err if last_err else TimeoutError(
        f"click_menu_path failed for {path!r}")


def list_top_menus(main_window) -> List[str]:
    """Return the list of top-level menu titles on the main window.

    Used by phase-2 tests to enumerate every menu before clicking
    each one. Qt exposes the menu bar with auto_id suffix
    ``.QMenuBar``; the main window also has a Windows system menu
    bar (Minimize/Maximize/Close) which we filter out by matching
    the Qt-specific auto_id.

    :param main_window: The pywinauto main window wrapper.
    :returns: List of titles in the order they appear on the bar.
    """
    # There are 2 MenuBar children on the main window: the Qt menu
    # bar and the Windows titlebar system menu. Walk children
    # manually and pick the one whose auto_id ends with ".QMenuBar".
    menubar = None
    for candidate in main_window.descendants(control_type="MenuBar"):
        try:
            aid = candidate.element_info.automation_id or ""
        except Exception:  # noqa: BLE001
            continue
        if aid.endswith(".QMenuBar") or aid == "QMenuBar":
            menubar = candidate
            break
    if menubar is None:
        raise RuntimeError("Qt menubar not found under main window")
    titles: List[str] = []
    for item in menubar.children(control_type="MenuItem"):
        try:
            t = item.window_text()
            if t and t != "System":
                titles.append(t)
        except Exception:  # noqa: BLE001
            continue
    return titles


def list_menu_leaves(main_window, top_title: str) -> List[str]:
    """Open a top-level menu and return the titles of its leaf items.

    Expands the menu, walks the CHILD ITEMS OF THE EXPANDED POPUP
    (not the entire main window), collects titles, and presses
    Escape to close the menu before returning.

    Karen phase-1 review finding #3: the previous implementation
    walked ``main_window.descendants(control_type="MenuItem")``,
    which picked up every top-level bar item in addition to the
    expanded popup's leaves. The top titles were only filtered by
    ``t != top_title``, so ``list_menu_leaves(win, "File")`` came
    back with ``["File-is-filtered", "New", "Open", "Edit",
    "View", ...]``. Now we find the ``Menu`` control that appeared
    after the click and walk only its children.

    :param main_window: The pywinauto main window wrapper.
    :param top_title: Name of the top-level menu.
    :returns: List of leaf titles in display order.
    """
    open_top_menu(main_window, top_title)
    titles: List[str] = []
    try:
        # After clicking a top bar item, Qt spawns a transient Menu
        # control (type="Menu") whose children are the leaves. It
        # appears as a top-level window on the Desktop, NOT as a
        # descendant of the main window — so we have to look across
        # the whole application, filtered by process ID.
        from pywinauto import Desktop
        menu_popup = None
        pid = main_window.process_id()
        for w in Desktop(backend="uia").windows():
            try:
                if w.process_id() != pid:
                    continue
                if w.element_info.control_type == "Menu":
                    menu_popup = w
                    break
            except Exception:  # noqa: BLE001
                continue

        if menu_popup is not None:
            for item in menu_popup.descendants(control_type="MenuItem"):
                try:
                    t = item.window_text()
                    if t and t not in titles:
                        titles.append(t)
                except Exception:  # noqa: BLE001
                    continue
        else:
            # Fallback: walk main_window descendants minus the
            # top-level bar titles. This is the pre-fix behavior,
            # kept as a fallback for Qt builds where the popup
            # lives inside the main window rather than on Desktop.
            top_titles = set(list_top_menus(main_window))
            for d in main_window.descendants(control_type="MenuItem"):
                try:
                    t = d.window_text()
                    if t and t not in titles and t not in top_titles:
                        titles.append(t)
                except Exception:  # noqa: BLE001
                    continue
    finally:
        # Close the menu cleanly so the next test doesn't inherit
        # an expanded menu.
        main_window.type_keys("{ESC}")
        time.sleep(0.1)
    return titles
