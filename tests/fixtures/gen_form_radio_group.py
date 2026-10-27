# -*- coding: utf-8 -*-
"""Generate tests/fixtures/form_radio_group.pdf.

A1-61 (meta-audit-2026-07 status register): the pre-existing
``RadioGroupExclusivity`` oracle skipped when the fixture happened to
carry fewer than two radio widgets and called itself "degenerate" when
the two it found might not share a group.  This fixture removes that
uncertainty by AUTHORING the exclusivity precondition:

  * ONE radio field ``Choice`` (``/FT /Btn`` with the radio flag
    ``1 << 15`` set, plus ``NoToggleToOff`` ``1 << 14`` so the group
    behaves like a real radio group).
  * EXACTLY TWO kid widgets under it, therefore unambiguously in the
    SAME group.
  * Authored, distinct export values: kid 0 -> ``/OptionA``,
    kid 1 -> ``/OptionB``.  Each kid's ``/AP /N`` holds exactly two
    states, ``/Off`` and its own export value, so MuPDF's
    ``pdf_button_field_on_state`` resolves the on-state deterministically.
  * Widget order in ``/Annots`` is A then B, and the rects are
    horizontally separated (A left of B) so a test can re-derive the
    ordering from geometry instead of trusting enumeration order.
  * Initial state: parent ``/V = /Off``, both kids ``/AS = /Off``.

Ground truth the tests anchor to (do NOT read these from the product):
    field name      "Choice"
    export values   "OptionA" (left widget), "OptionB" (right widget)
    initial value   "Off" for the group and both widgets

Regenerate with:  python tests/fixtures/gen_form_radio_group.py
"""
from __future__ import annotations

import os

import pikepdf
from pikepdf import Array, Dictionary, Name, String

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = os.path.join(HERE, "form_radio_group.pdf")

PAGE_W, PAGE_H = 612, 792

# Authored geometry (PDF user space, Y-up). Widget A is to the LEFT of B.
RECT_A = [100, 700, 120, 720]
RECT_B = [200, 700, 220, 720]

EXPORT_A = "OptionA"
EXPORT_B = "OptionB"

# Radio field flags: bit 16 (1<<15) Radio, bit 15 (1<<14) NoToggleToOff.
FF_RADIO = (1 << 15) | (1 << 14)

# Appearance streams. The "on" state paints a filled disc, "off" paints only
# the ring, so the two states are visually distinct as well as structurally.
_AP_OFF = b"0.2 w 10 10 m 10 10 8 0 360 arc S"
_AP_ON = b"0 0 0 rg 10 10 8 0 360 arc f"


def _ap_stream(pdf: pikepdf.Pdf, content: bytes) -> pikepdf.Object:
    return pdf.make_indirect(
        pikepdf.Stream(
            pdf,
            content,
            Type=Name.XObject,
            Subtype=Name.Form,
            BBox=Array([0, 0, 20, 20]),
            Resources=Dictionary(),
        )
    )


def main() -> int:
    pdf = pikepdf.new()
    page = pdf.add_blank_page(page_size=(PAGE_W, PAGE_H))

    parent = pdf.make_indirect(
        Dictionary(
            FT=Name.Btn,
            T=String("Choice"),
            Ff=FF_RADIO,
            V=Name.Off,
            DV=Name.Off,
        )
    )

    kids = []
    for rect, export in ((RECT_A, EXPORT_A), (RECT_B, EXPORT_B)):
        ap_n = Dictionary()
        ap_n[Name("/Off")] = _ap_stream(pdf, _AP_OFF)
        ap_n[Name("/" + export)] = _ap_stream(pdf, _AP_ON)
        kid = pdf.make_indirect(
            Dictionary(
                Type=Name.Annot,
                Subtype=Name.Widget,
                Rect=Array(rect),
                F=4,  # Print
                Parent=parent,
                AS=Name.Off,
                AP=Dictionary(N=ap_n),
                MK=Dictionary(BC=Array([0, 0, 0]), CA=String("l")),
            )
        )
        kids.append(kid)

    parent[Name("/Kids")] = Array(kids)

    page.Annots = Array(kids)
    pdf.Root.AcroForm = Dictionary(
        Fields=Array([parent]),
        DA=String("/Helv 0 Tf 0 g"),
        DR=Dictionary(
            Font=Dictionary(
                Helv=pdf.make_indirect(
                    Dictionary(
                        Type=Name.Font,
                        Subtype=Name.Type1,
                        BaseFont=Name.Helvetica,
                        Encoding=Name.WinAnsiEncoding,
                    )
                )
            )
        ),
    )

    pdf.save(OUTPUT)
    print("wrote", OUTPUT, os.path.getsize(OUTPUT), "bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
