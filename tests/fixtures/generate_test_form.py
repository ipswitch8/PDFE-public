# -*- coding: utf-8 -*-
"""Generate tests/fixtures/test_form.pdf -- GitHub #210 (register row A5-20).

Regenerate with:  python tests/fixtures/generate_test_form.py

WHY THIS EXISTS

`test_form.pdf` was a tracked binary with NO generator. It is also the INPUT to
two other generators -- `generate_form_with_dv.py:24` reads it, and
`gen_f15_form_data_fdf.py` takes its field names -- so a `git clean` of binary
fixtures cost five fixtures, not three: `test_form_with_dv.pdf` and
`f15_form_data.fdf` went with it. `phase-61-dispositions.md` confirmed this and
recorded it as "converted to a tracked follow-up"; it was never tracked
anywhere, which under docs/TRACKING.md is a silent drop.

WHY IT WAS NOT WRITTEN EARLIER, AND WHY THAT REASONING IS RESPECTED HERE

phase-61 declined because several suites assert against this fixture's exact
field set, and a subtly-wrong regeneration converts a KNOWN regeneration gap
into SILENT fixture drift -- strictly worse. So the specification below was not
invented: it was extracted from the committed binary
(`tests/fixtures/form_field_inventory.py`), and
`tests/fixtures/verify_form_fixture_generators.py` re-extracts it from a
freshly generated file and compares field-by-field. The generator is only
trustworthy because that comparison exists; do not change one without the other.

THE AUTHORED GROUND TRUTH (what the suites read)

  * 1 page, MediaBox [0 0 612 792], /Rotate 0, 9 widget annotations.
  * AcroForm /DA "/Helv 0 Tf 0 g", /DR /Font /Helv (Type1 Helvetica).
  * NO /CO -- `test_calculation_order.cpp:52` asserts getCalculationOrder() is
    EMPTY. A generator that invents a calculation order reddens that test.
  * NO /AA and no /Names /JavaScript anywhere: this fixture carries no script.
  * 8 top-level fields; `test_ui_statusbar.py` asserts exactly 8.

  FirstName   /Tx  Ff 0      V ""   DV ""   MaxLen 100  Rect [180 690 380 710]
  LastName    /Tx  Ff 0      V ""   DV ""   MaxLen 100  Rect [180 650 380 670]
  Comments    /Tx  Ff 4096   V ""   DV ""   MaxLen 100  Rect [180 580 380 630]
  AgreeTerms  /Btn Ff 2      V /Off AS /Off             Rect [180 540 200 560]
  Subscribe   /Btn Ff 2      V /Yes AS /Yes             Rect [180 500 200 520]
  Gender      /Btn Ff 49154  (radio parent, no /V), two kid widgets:
                             /Male   AS /Off            Rect [180 460 200 480]
                             /Female AS /Off            Rect [260 460 280 480]
  Country     /Ch  Ff 131072 V "USA"  DV "USA"          Rect [180 420 380 440]
                             /Opt [USA Canada UK Australia Germany]
  Interests   /Ch  Ff 0      V "Programming" DV same    Rect [180 330 380 390]
                             /Opt [Programming Music Sports Reading Travel]

  Every widget carries /F 4 (Print), /BS << /S /S /W 1 >> and
  /MK << /BC [0 0 0] /BG [1 1 1] >>. The text and choice widgets add
  /DA "/Helv 10 Tf 0 0 0 rg"; the check boxes add /MK /CA "4" (ZapfDingbats
  check) and the radios /MK /CA "l" (ZapfDingbats filled circle).
  `test_form_appearance_render.cpp:70-96` measures INK inside the FirstName
  widget against that exact rect plus that exact /MK + /BS pair, so none of
  those four things is decorative.

BYTE IDENTITY IS NOT CLAIMED AND IS NOT THE POINT. A faithful regeneration
still differs from the committed binary in object numbering, stream filters and
the producer string. The verifier compares the FIELD INVENTORY -- what the
suites actually assert on -- because a byte diff would report a difference for
every correct regeneration and so could never distinguish drift from noise.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

import pikepdf
from pikepdf import Array, Dictionary, Name, String

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = Path(__file__).resolve().parent
OUT = HERE / "test_form.pdf"

MEDIABOX = [0, 0, 612, 792]
TEXT_DA = "/Helv 10 Tf 0 0 0 rg"

# Ff bits, spelled out so a reader does not have to decode 49154 by hand.
FF_READONLY = 1 << 0
FF_REQUIRED = 1 << 1          # 2 -- set on both check boxes in this fixture
FF_MULTILINE = 1 << 12        # 4096 -- Comments
FF_RADIO = 1 << 15            # 32768
FF_PUSHBUTTON = 1 << 16       # 65536
FF_COMBO = 1 << 17            # 131072 -- Country
FF_GENDER = FF_RADIO | (1 << 14) | FF_REQUIRED  # 49154, as authored

assert FF_GENDER == 49154, "radio-group flag word drifted from the fixture"


def _mk(with_ca=None):
    d = Dictionary(BC=Array([0, 0, 0]), BG=Array([1, 1, 1]))
    if with_ca is not None:
        d[Name("/CA")] = String(with_ca)
    return d


def _bs():
    return Dictionary(S=Name("/S"), W=1)


def _blank_stream(pdf, rect):
    """A minimal, valid /N appearance stream sized to ``rect``.

    Draws the authored 1pt black border on a white ground -- the same thing the
    committed fixture's stream draws, and the thing
    test_form_appearance_render.cpp measures as "ink at the edge, none in the
    interior" for an EMPTY text field.
    """
    w = float(rect[2]) - float(rect[0])
    h = float(rect[3]) - float(rect[1])
    content = (
        "q\n"
        "1 1 1 rg\n"
        "0 0 %.2f %.2f re f\n"
        "0 0 0 RG\n"
        "1 w\n"
        "0.5 0.5 %.2f %.2f re S\n"
        "Q\n" % (w, h, w - 1.0, h - 1.0)
    ).encode("ascii")
    stream = pikepdf.Stream(pdf, content)
    stream[Name("/Type")] = Name("/XObject")
    stream[Name("/Subtype")] = Name("/Form")
    stream[Name("/BBox")] = Array([0, 0, w, h])
    stream[Name("/Resources")] = Dictionary()
    return pdf.make_indirect(stream)


def _state_stream(pdf, rect, on):
    """An appearance stream for one check/radio state."""
    w = float(rect[2]) - float(rect[0])
    h = float(rect[3]) - float(rect[1])
    body = "q\n1 1 1 rg\n0 0 %.2f %.2f re f\n0 0 0 RG\n1 w\n" \
           "0.5 0.5 %.2f %.2f re S\n" % (w, h, w - 1.0, h - 1.0)
    if on:
        # A simple X, so an "on" state is visually distinguishable from "off".
        body += "2 2 m %.2f %.2f l S\n2 %.2f m %.2f 2 l S\n" % (
            w - 2.0, h - 2.0, h - 2.0, w - 2.0)
    body += "Q\n"
    stream = pikepdf.Stream(pdf, body.encode("ascii"))
    stream[Name("/Type")] = Name("/XObject")
    stream[Name("/Subtype")] = Name("/Form")
    stream[Name("/BBox")] = Array([0, 0, w, h])
    stream[Name("/Resources")] = Dictionary()
    return pdf.make_indirect(stream)


def _appearance_states(pdf, rect, on_name):
    """/AP with /N, /D and /R sub-dictionaries keyed by state name.

    All three are present in the committed fixture. They are reproduced because
    the inventory compares which sub-dictionaries and state names EXIST, and
    because a viewer that finds /D missing falls back silently -- a difference
    that would show up only as a rendering change nobody is asserting on.
    """
    def states():
        return Dictionary(**{
            on_name.lstrip("/"): _state_stream(pdf, rect, True),
            "Off": _state_stream(pdf, rect, False),
        })
    return Dictionary(N=states(), D=states(), R=states())


def build(pdf, page):
    """Create the 8 top-level fields; return (fields, annots)."""
    fields = []
    annots = []

    def text_field(name, rect, ff=0):
        w = pdf.make_indirect(Dictionary(
            Type=Name("/Annot"), Subtype=Name("/Widget"),
            FT=Name("/Tx"), T=String(name), Ff=ff,
            V=String(""), DV=String(""), MaxLen=100,
            Rect=Array(rect), F=4, DA=String(TEXT_DA),
            MK=_mk(), BS=_bs(), P=page.obj,
            AP=Dictionary(N=_blank_stream(pdf, rect)),
        ))
        fields.append(w)
        annots.append(w)

    text_field("FirstName", [180, 690, 380, 710])
    text_field("LastName", [180, 650, 380, 670])
    text_field("Comments", [180, 580, 380, 630], ff=FF_MULTILINE)

    def check_box(name, rect, value):
        w = pdf.make_indirect(Dictionary(
            Type=Name("/Annot"), Subtype=Name("/Widget"),
            FT=Name("/Btn"), T=String(name), Ff=FF_REQUIRED,
            V=Name(value), AS=Name(value),
            Rect=Array(rect), F=4,
            MK=_mk(with_ca="4"), BS=_bs(), P=page.obj,
            AP=_appearance_states(pdf, rect, "/Yes"),
        ))
        fields.append(w)
        annots.append(w)

    check_box("AgreeTerms", [180, 540, 200, 560], "/Off")
    check_box("Subscribe", [180, 500, 200, 520], "/Yes")

    # Gender: ONE field, TWO widget kids. The kids carry no /T of their own, so
    # both resolve to the qualified name "Gender" -- that is what makes it a
    # radio GROUP rather than two independent buttons, and it is why the
    # inventory keys fields on (name, rect) instead of name alone.
    kids = []
    for rect, export in (([180, 460, 200, 480], "/Male"),
                         ([260, 460, 280, 480], "/Female")):
        kid = pdf.make_indirect(Dictionary(
            Type=Name("/Annot"), Subtype=Name("/Widget"),
            AS=Name("/Off"), Rect=Array(rect), F=4,
            MK=_mk(with_ca="l"), BS=_bs(), P=page.obj,
            AP=_appearance_states(pdf, rect, export),
        ))
        kids.append(kid)
        annots.append(kid)
    gender = pdf.make_indirect(Dictionary(
        FT=Name("/Btn"), T=String("Gender"), Ff=FF_GENDER,
        Kids=Array(kids),
    ))
    for kid in kids:
        kid[Name("/Parent")] = gender
    fields.append(gender)

    def choice_field(name, rect, options, value, ff):
        w = pdf.make_indirect(Dictionary(
            Type=Name("/Annot"), Subtype=Name("/Widget"),
            FT=Name("/Ch"), T=String(name), Ff=ff,
            V=String(value), DV=String(value),
            Opt=Array([String(o) for o in options]),
            Rect=Array(rect), F=4, DA=String(TEXT_DA),
            MK=_mk(), BS=_bs(), P=page.obj,
            AP=Dictionary(N=_blank_stream(pdf, rect)),
        ))
        fields.append(w)
        annots.append(w)

    choice_field("Country", [180, 420, 380, 440],
                 ["USA", "Canada", "UK", "Australia", "Germany"],
                 "USA", FF_COMBO)
    choice_field("Interests", [180, 330, 380, 390],
                 ["Programming", "Music", "Sports", "Reading", "Travel"],
                 "Programming", 0)

    return fields, annots


def main(out_path=None):
    out = Path(out_path) if out_path else OUT
    pdf = pikepdf.new()
    page = pdf.add_blank_page(page_size=(612, 792))
    page.obj[Name("/Rotate")] = 0
    page.obj[Name("/MediaBox")] = Array(MEDIABOX)

    helv = pdf.make_indirect(Dictionary(
        Type=Name("/Font"), Subtype=Name("/Type1"),
        BaseFont=Name("/Helvetica"), Encoding=Name("/WinAnsiEncoding"),
    ))

    fields, annots = build(pdf, page)
    page.obj[Name("/Annots")] = Array(annots)

    pdf.Root[Name("/AcroForm")] = pdf.make_indirect(Dictionary(
        Fields=Array(fields),
        DA=String("/Helv 0 Tf 0 g"),
        DR=Dictionary(Font=Dictionary(Helv=helv)),
    ))
    # Deliberately absent: /CO (test_calculation_order.cpp asserts empty),
    # /NeedAppearances (the fixture ships real /AP streams), /SigFlags.

    pdf.save(str(out))
    print("wrote", out, os.path.getsize(out), "bytes;",
          len(fields), "top-level fields,", len(annots), "widgets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1] if len(sys.argv) > 1 else None))
