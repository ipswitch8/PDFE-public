# -*- coding: utf-8 -*-
"""Canonical AcroForm field inventory -- the comparison basis for GitHub #210.

WHY A SEPARATE MODULE RATHER THAN A DIFF.

`test_form.pdf`, `test_form_js.pdf` and `test_form_with_pushbutton.pdf` had no
generator (register row A5-20, "converted to a tracked follow-up" that was never
tracked). Writing one raises the question the phase-61 disposition correctly
refused to answer casually: how do you know the regenerated file is the same
fixture? Byte comparison answers NO for any faithful regeneration -- object
numbering, stream filters and the producer string all differ -- so a byte diff
is useless as an oracle and its failure would be indistinguishable from real
drift.

What the suites actually assert on is the FIELD INVENTORY: names, types, flags,
values, defaults, options, widget rectangles, appearance states, and the border
and appearance-characteristic dictionaries `test_form_appearance_render.cpp`
measures ink against. This module extracts exactly that, canonically, so a
regenerated fixture can be compared field-by-field as GitHub #210 asks.

Deliberately EXCLUDED from the inventory, with reasons:

  /P, /Parent, /Kids back-references
      Structural cycles. Following them recurses into the page, its /Parent
      page-tree node, and back to the annotation -- the shape that made a naive
      dump of the 649-byte pushbutton fixture unreadable. Field->widget kinship
      is preserved instead by the flattened, fully-qualified /T path.

  /AP stream BYTES **and LENGTH**
      An appearance stream is a rendering, not a specification. Two generators
      can draw the same border with different operators, and the committed
      fixture's own streams vary from 74 to 299 bytes for widgets the suites
      treat identically. The inventory records which appearance
      sub-dictionaries and states EXIST (/N, /D, /R and their state names) and
      whether the stream is NON-EMPTY -- because a zero-length appearance is a
      real defect and everything above that is a drawing choice.

      This is a deliberate limit on what the verifier can prove, and it is
      stated rather than hidden: structural equivalence is checked here;
      whether the FirstName widget still renders ink at its border and none in
      its interior is checked by
      `tests/test_form_appearance_render.cpp:70-96`, against the real
      renderer. Neither substitutes for the other.

Import path: this module is imported by `verify_form_fixture_generators.py`.
It has no side effects and writes nothing.
"""
from __future__ import annotations

import sys

import pikepdf

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# Keys copied verbatim (scalar or simple-array) from a field/widget dict.
_SCALAR_KEYS = ("/FT", "/Ff", "/MaxLen", "/Q", "/DA", "/AS", "/F", "/TU")
# Keys whose value is a text/name value needing normalisation.
_VALUE_KEYS = ("/V", "/DV")


def _plain(obj):
    """Render a pikepdf object as a comparable plain-Python value."""
    if obj is None:
        return None
    if isinstance(obj, pikepdf.Name):
        return str(obj)
    if isinstance(obj, pikepdf.String):
        return str(obj)
    if isinstance(obj, pikepdf.Array):
        return [_plain(v) for v in obj]
    if isinstance(obj, pikepdf.Stream):
        # Streams appear here only as /AP leaves; see the module docstring for
        # why the bytes are not an equality key.
        return {"__stream_nonempty__": len(bytes(obj.read_bytes())) > 0}
    if isinstance(obj, pikepdf.Dictionary):
        return {str(k): _plain(v) for k, v in obj.items()}
    # Numbers / booleans / null.
    try:
        return float(obj) if "." in repr(obj) else int(obj)
    except (TypeError, ValueError):
        return repr(obj)


def _rect(obj):
    """A /Rect as four rounded numbers, or None."""
    if obj is None:
        return None
    return [round(float(v), 3) for v in obj]


def _appearance_shape(ap):
    """Which appearance dictionaries and states exist, not what they draw."""
    if ap is None:
        return None
    shape = {}
    for key in ("/N", "/D", "/R"):
        sub = ap.get(key)
        if sub is None:
            continue
        if isinstance(sub, pikepdf.Stream):
            shape[key] = {"kind": "stream",
                          "nonempty": len(bytes(sub.read_bytes())) > 0}
        elif isinstance(sub, pikepdf.Dictionary):
            shape[key] = {"kind": "states",
                          "states": sorted(str(k) for k in sub.keys())}
        else:
            shape[key] = {"kind": "other"}
    return shape


def _qualified_name(field, parent_name):
    t = field.get("/T")
    part = str(t) if t is not None else None
    if part is None:
        return parent_name
    return part if not parent_name else "%s.%s" % (parent_name, part)


def _walk(field, parent_name, out, inherited_ft, inherited_ff):
    """Flatten a field subtree into fully-qualified leaf entries."""
    name = _qualified_name(field, parent_name)
    ft = str(field["/FT"]) if "/FT" in field else inherited_ft
    ff = int(field["/Ff"]) if "/Ff" in field else inherited_ff

    kids = field.get("/Kids")
    # A /Kids array of WIDGETS (no /T of their own) is one field with several
    # appearances -- a radio group. A /Kids array of named fields is a true
    # hierarchy. Both are flattened; the distinction shows up in the name.
    if kids is not None and len(kids) > 0:
        for kid in kids:
            _walk(kid, name, out, ft, ff)
        # A parent that also carries a value (radio group /V) is recorded too,
        # because resetForm and FDF import both address it by name.
        if "/V" in field or "/DV" in field:
            out.append(_leaf(field, name, ft, ff, widget=False))
        return

    out.append(_leaf(field, name, ft, ff, widget=("/Subtype" in field)))


def _leaf(field, name, ft, ff, widget):
    entry = {
        "name": name,
        "ft": ft,
        "ff": ff,
        "is_widget": bool(widget),
        "rect": _rect(field.get("/Rect")),
        "ap": _appearance_shape(field.get("/AP")),
        "opt": _plain(field.get("/Opt")),
        "mk": _plain(field.get("/MK")),
        "bs": _plain(field.get("/BS")),
    }
    for key in _SCALAR_KEYS:
        entry[key.lstrip("/").lower()] = _plain(field.get(key))
    for key in _VALUE_KEYS:
        entry[key.lstrip("/").lower()] = _plain(field.get(key))
    # /Ff and /FT are already resolved (possibly inherited) above.
    entry.pop("ft", None)
    entry["ft"] = ft
    entry["ff"] = ff
    return entry


def inventory(path):
    """Return the canonical inventory dict for the PDF at ``path``."""
    with pikepdf.open(str(path)) as pdf:
        pages = []
        for page in pdf.pages:
            pages.append({
                "mediabox": _rect(page.obj.get("/MediaBox")),
                "rotate": _plain(page.obj.get("/Rotate")),
                "annot_count": len(page.obj.get("/Annots", [])),
            })
        acro = pdf.Root.get("/AcroForm")
        fields = []
        acro_summary = None
        if acro is not None:
            acro_summary = {
                "da": _plain(acro.get("/DA")),
                "need_appearances": _plain(acro.get("/NeedAppearances")),
                "sig_flags": _plain(acro.get("/SigFlags")),
                "q": _plain(acro.get("/Q")),
                # /DR is compared by the FONT NAMES it publishes: the suites
                # resolve "/Helv" out of it, and a resource dictionary that
                # still answers /Helv is the same resource dictionary for
                # every purpose a test has.
                "dr_fonts": sorted(
                    str(k) for k in acro.get("/DR", {}).get("/Font", {}).keys()
                ) if "/DR" in acro else None,
                # /CO is load-bearing: test_calculation_order.cpp asserts it is
                # EMPTY for test_form.pdf. A generator that invents one would
                # redden that test, so it is compared, not ignored.
                "co_count": len(acro.get("/CO", [])) if "/CO" in acro else 0,
                "top_level_field_count": len(acro.get("/Fields", [])),
            }
            for field in acro.get("/Fields", []):
                _walk(field, "", fields, None, 0)
        fields.sort(key=lambda e: (e["name"] or "", str(e["rect"])))
        return {"pages": pages, "acroform": acro_summary, "fields": fields}


def diff(expected, actual):
    """Return a list of human-readable difference strings ([] when equal)."""
    problems = []

    if expected.get("pages") != actual.get("pages"):
        problems.append("pages differ:\n  expected %r\n  actual   %r"
                        % (expected.get("pages"), actual.get("pages")))

    exp_acro = expected.get("acroform") or {}
    act_acro = actual.get("acroform") or {}
    for key in sorted(set(exp_acro) | set(act_acro)):
        if exp_acro.get(key) != act_acro.get(key):
            problems.append("AcroForm %s differs: expected %r, actual %r"
                            % (key, exp_acro.get(key), act_acro.get(key)))

    exp_by_key = {(f["name"], str(f["rect"])): f for f in expected["fields"]}
    act_by_key = {(f["name"], str(f["rect"])): f for f in actual["fields"]}
    for key in sorted(set(exp_by_key) - set(act_by_key)):
        problems.append("field MISSING from regenerated file: %s rect=%s"
                        % key)
    for key in sorted(set(act_by_key) - set(exp_by_key)):
        problems.append("field UNEXPECTED in regenerated file: %s rect=%s"
                        % key)
    for key in sorted(set(exp_by_key) & set(act_by_key)):
        exp_f, act_f = exp_by_key[key], act_by_key[key]
        for attr in sorted(set(exp_f) | set(act_f)):
            if exp_f.get(attr) != act_f.get(attr):
                problems.append(
                    "field %s rect=%s: %s differs: expected %r, actual %r"
                    % (key[0], key[1], attr, exp_f.get(attr), act_f.get(attr)))
    return problems


if __name__ == "__main__":
    import json
    for arg in sys.argv[1:]:
        print("=" * 70)
        print(arg)
        print(json.dumps(inventory(arg), indent=1, sort_keys=True))
