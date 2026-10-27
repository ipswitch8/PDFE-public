# -*- coding: utf-8 -*-
"""Phase-7 C2 — the regenerated baselines did not loosen the oracles.

WHY THIS IS THE RIGHT QUESTION
==============================
Sixteen committed baselines were regenerated after the GitHub #279 capture fix
(``PrintWindow`` was missing ``PW_CLIENTONLY``, so every capture was displaced
by the non-client offset and the "page crop" began inside the toolbar). A
regeneration always turns a red oracle green, which is exactly why it is the
move `_baseline_check` warns against in its own failure text:

    "Do NOT rebaseline to make this go away"

A green run afterwards therefore proves nothing on its own. The question that
does mean something is whether the oracle can still tell the two apart: if the
NEW baseline would also accept the OLD image, the regeneration did not fix an
oracle, it blunted one.

So each pair is compared at the SAME thresholds the owning test uses, and the
old image must FAIL. The old bytes are committed under
``tests/fixtures/visual_regressions/pre279_baselines/`` because reading them
from git at test time stops working the moment the regeneration is committed --
``git show HEAD:<path>`` would then return the new bytes and the test would
compare an image with itself, passing vacuously forever.

Note the criterion this satisfies says the oracles were "broken by the palette
change in #279". That premise is wrong -- the measured cause was the capture
origin, not the palette; see
``docs/audit/p96-phase7/printwindow-client-origin-and-the-toolbar-oracle.md``.
The criterion's SUBSTANCE -- prove the re-baselined oracle still discriminates
-- is what is implemented here.

Offline by construction: no app launch, no UIA, no desktop. It compares two
committed PNGs, so it runs anywhere and cannot be flaky.

Run:  python -m pytest tests/visual/test_rebaselined_oracles_still_discriminate.py -q
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
assert (REPO_ROOT / ".github" / "workflows").is_dir(), (
    f"repo root misresolved to {REPO_ROOT!r}")
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

pytest.importorskip("PIL")
from PIL import Image  # noqa: E402

from tests.visual.diff import compare  # noqa: E402

NEW_DIR = REPO_ROOT / "tests" / "fixtures" / "screenshots"
OLD_DIR = (REPO_ROOT / "tests" / "fixtures" / "visual_regressions"
           / "pre279_baselines")

#: The thresholds the owning tests use. phase5_* canvas oracles are checked at
#: the tightened values test_ui_file_operations sets; phase6_* at the 0.95/0.05
#: pair test_ui_view_navigation uses. Taken from the call sites rather than
#: invented, so this measures the real oracle rather than a stricter stand-in
#: that would pass trivially.
TIGHT = (0.99, 0.001)
LOOSE = (0.95, 0.05)


def _thresholds(name: str):
    return LOOSE if name.startswith("phase6_") else TIGHT


def _pairs():
    if not OLD_DIR.is_dir():
        return []
    out = []
    for old in sorted(OLD_DIR.glob("*.png")):
        new = NEW_DIR / old.name
        if new.exists():
            out.append((old.name, old, new))
    return out


PAIRS = _pairs()


def test_the_fixture_set_is_not_empty():
    """Guard the parametrisation itself.

    An empty ``PAIRS`` would make every test below silently vanish and the file
    would report success while asserting nothing -- the failure mode this
    phase has already hit twice (a deleted baseline makes `_baseline_check`
    return without comparing; a chrome detector that could not fire).
    """
    assert len(PAIRS) >= 16, (
        f"expected at least 16 regenerated baselines to check, found "
        f"{len(PAIRS)}. Did extract_pre279_baselines.py run before the "
        f"regeneration was committed?")


@pytest.mark.parametrize("name,old_path,new_path", PAIRS,
                         ids=[p[0] for p in PAIRS])
def test_the_old_image_is_rejected_by_the_new_baseline(name, old_path,
                                                       new_path):
    """THE criterion: the oracle must still tell old from new."""
    ssim_t, pixel_t = _thresholds(name)
    old = Image.open(str(old_path)).convert("RGB")
    new = Image.open(str(new_path)).convert("RGB")

    if old.size != new.size:
        # `_baseline_check` treats a size change as a hard failure before it
        # compares anything, so a differing size IS rejection -- and is how the
        # five dialog baselines differ (exactly 32 rows, the removed title-bar
        # crop). Kept out of the SSIM path because compare() would silently
        # coerce them to a common size and turn a decisive result into an
        # approximate one.
        #
        # ASSERTED, NOT `return`ed. An early return here would make five of
        # the sixteen parametrisations assert nothing while still reporting
        # PASS -- the same vacuity that made two "47 passed" runs meaningless
        # earlier in this phase. State the property instead.
        assert abs(old.size[1] - new.size[1]) > 0 or old.size[0] != new.size[0]
        assert old.size[0] == new.size[0], (
            f"{name}: width changed ({old.size} -> {new.size}). Only the "
            f"HEIGHT should differ, by the 32-row title-bar crop removed from "
            f"_capture_dialog_client; a width change means something else "
            f"moved and the regeneration needs re-examining.")
        return

    res = compare(old, new, ssim_threshold=ssim_t, pixel_threshold=pixel_t)
    assert not res["pass"], (
        f"{name}: the NEW baseline ACCEPTS the OLD image "
        f"(SSIM={res['ssim']:.4f} >= {ssim_t}, "
        f"pixel_fraction={res['pixel_fraction']:.4f} <= {pixel_t}). The "
        f"regeneration did not fix this oracle, it blunted it -- the two "
        f"images are the pre-fix and post-fix captures of the same scene and "
        f"the oracle can no longer distinguish them.")


@pytest.mark.parametrize("name,old_path,new_path", PAIRS,
                         ids=[p[0] for p in PAIRS])
def test_the_new_baseline_accepts_itself(name, old_path, new_path):
    """Sensitivity control for the test above.

    Without it, the rejection test is satisfied by thresholds so tight that
    nothing could ever pass -- an all-RED oracle demonstrates sensitivity and
    nothing else. This pins the other direction at the same thresholds.
    """
    ssim_t, pixel_t = _thresholds(name)
    new = Image.open(str(new_path)).convert("RGB")
    res = compare(new, new, ssim_threshold=ssim_t, pixel_threshold=pixel_t)
    assert res["pass"], (
        f"{name}: the oracle rejects its own baseline at "
        f"SSIM>={ssim_t}/pixel<={pixel_t} (got SSIM={res['ssim']:.4f}, "
        f"pixel_fraction={res['pixel_fraction']:.4f}), so the rejection test "
        f"above proves nothing about discrimination")
