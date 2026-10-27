# -*- coding: utf-8 -*-
"""Flaky-test retry decorator.

Qt + pywinauto + RDP is a cocktail that produces occasional
transient failures that are not real bugs: WaitForInputIdle hangs,
element-not-available errors during window refresh, clipboard
reads returning empty strings because the RDP session manager has
briefly taken ownership. These are the three failure modes the
production TextEditTool worked around in commit 8371c81.

The ``@retry`` decorator absorbs these transients. Used sparingly:
every test that fires it gets a note in the matrix CSV's ``notes``
column so we can track which tests are provably flaky vs which are
bug detectors.

Usage::

    from tests.ui_automation.harness.retry import retry

    @retry(max_attempts=3)
    def test_open_encrypted_doc(self):
        ...
"""
from __future__ import annotations

import functools
import time
from typing import Callable, Tuple, Type


#: Exception types and substring patterns that indicate a transient
#: failure worth retrying. Anything not in this list is re-raised
#: immediately — real bugs should fail loud on the first try.
TRANSIENT_EXCEPTION_PATTERNS: Tuple[str, ...] = (
    "WaitForInputIdle",
    "ElementNotAvailable",
    "element_info",
    "NotAnElement",
    "cannot find window",
    "Clipboard",
    "OpenClipboard",
    "timed out",
)


def _is_transient(err: BaseException) -> bool:
    """Return True iff the exception message matches a known
    transient-failure pattern."""
    msg = str(err) + " " + type(err).__name__
    return any(p.lower() in msg.lower() for p in TRANSIENT_EXCEPTION_PATTERNS)


def retry(
    max_attempts: int = 3,
    delay_s: float = 0.5,
    backoff: float = 1.5,
    exceptions: Tuple[Type[BaseException], ...] = (Exception,),
) -> Callable:
    """Re-run the decorated function on transient failures.

    :param max_attempts: Total attempts including the first.
    :param delay_s: Initial delay between attempts in seconds.
    :param backoff: Multiplier applied to the delay after each
                    failed attempt.
    :param exceptions: Exception classes that are candidates for
                       retry. Anything outside this tuple always
                       propagates immediately. Inside the tuple, a
                       second filter (:func:`_is_transient`)
                       applies so genuine bugs don't get masked.
    """
    def decorator(fn: Callable) -> Callable:
        @functools.wraps(fn)
        def wrapper(*args, **kwargs):
            last_err: BaseException | None = None
            current_delay = delay_s
            for attempt in range(1, max_attempts + 1):
                try:
                    return fn(*args, **kwargs)
                except exceptions as e:
                    last_err = e
                    if not _is_transient(e):
                        raise
                    if attempt == max_attempts:
                        raise
                    time.sleep(current_delay)
                    current_delay *= backoff
            # Unreachable — loop either returns or raises.
            if last_err:
                raise last_err
            raise RuntimeError("retry() exited without result or error")
        return wrapper
    return decorator
