# -*- coding: utf-8 -*-
"""After restoring a mutated source, PROVE the rebuild happened (F69b).

THE DEFECT THIS GENERALISES. A mutate/restore harness that puts the original
bytes back with a `shutil.copy2`-style copy also restores the original MTIME.
The build system then sees a source older than the object it already has, skips
it, and leaves a binary that is still half-mutated -- while every subsequent
step reports on it as though it were clean. A run after that measures a tree
nobody has: the source says one thing and the binary says another.

WHY NOT JUST TOUCH THE FILE. That was the first fix, made inside one throwaway
script, and the backlog row (F69) is explicit that "a fix inside one throwaway
script is not a control". Stamping `os.utime` also only addresses the mechanism
that was noticed. This asserts the PROPERTY instead -- the binary postdates the
sources it was built from -- which holds regardless of how the restore was done,
including restores that do not go through Python at all.

It is the same property `tests/scripts/run_gtest.py` already enforces as its
staleness preflight before a suite run. Stated once, here, so a harness does not
rediscover it per-phase.

Usage:
    ok, msg = binary_postdates_sources(binary, [src1, src2])
    if not ok: raise SystemExit(msg)
"""
from __future__ import annotations

import os
from typing import Iterable


def newest_mtime(paths: Iterable[str]) -> tuple[float, str]:
    """(mtime, path) of the most recently modified existing path. (-1, '') if none."""
    best = (-1.0, "")
    for p in paths:
        try:
            m = os.path.getmtime(p)
        except OSError:
            continue
        if m > best[0]:
            best = (m, p)
    return best


def binary_postdates_sources(binary: str, sources: Iterable[str]
                             ) -> tuple[bool, str]:
    """True iff `binary` exists and is at least as new as every source.

    Returns (ok, message). The message names the offending source and both
    timestamps, because "stale binary" without saying WHICH file sends someone
    diffing a whole tree.

    A MISSING BINARY IS A FAILURE, not a pass. An absent artifact cannot be
    proved fresh, and treating "nothing to compare" as success is how a check
    that never runs reports green -- this repository's most repeated defect.
    """
    sources = list(sources)
    if not sources:
        return False, ("no sources given to compare against -- refusing to "
                       "report freshness against an empty set, which would "
                       "pass vacuously")
    try:
        bmtime = os.path.getmtime(binary)
    except OSError:
        return False, ("binary %r does not exist, so the rebuild demonstrably "
                       "did not happen" % binary)

    smtime, spath = newest_mtime(sources)
    if smtime < 0:
        return False, ("none of the %d source path(s) exist, so there is "
                       "nothing to prove freshness against" % len(sources))

    if bmtime < smtime:
        return False, (
            "STALE BINARY: %r (mtime %.0f) is OLDER than %r (mtime %.0f). "
            "The restore put the original bytes back but the rebuild did not "
            "happen -- most likely the restore preserved the source's original "
            "mtime, so the build system skipped it and the binary is still "
            "half-mutated. Everything measured against it from here is "
            "measuring a tree that does not exist (F69)."
            % (binary, bmtime, spath, smtime))

    return True, ("binary %r postdates all %d source(s); newest is %r"
                  % (binary, len(sources), spath))
