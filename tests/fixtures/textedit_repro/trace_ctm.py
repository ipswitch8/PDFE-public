# -*- coding: utf-8 -*-
"""Trace CTM through the saved page-3 content stream from byte 0 up to
the inserted /Fsub1 BT, so we know the EXACT CTM in effect at our
rewrite injection point. Compare it to the ctm logged by the textFilter
callback (ctm.a=0.48, ctm.d=-0.48). If they differ, the math chain
that recovers Tm = trm * ctm_inv was inverting against the WRONG ctm
and the resulting Tm has the wrong sign or scale.
"""
from __future__ import annotations

import pathlib
import re

DATA = pathlib.Path(__file__).parent / "edited_page3_content.bin"
text = DATA.read_bytes().decode("latin-1")


def mul(a, b):
    """fz_concat-like: a then b, MuPDF convention.
    Each matrix is (m_a, m_b, m_c, m_d, m_e, m_f) representing affine
    (x', y') = (m_a*x + m_c*y + m_e, m_b*x + m_d*y + m_f).
    fz_concat(one, two) returns the matrix for 'apply one then two'."""
    one = a; two = b
    da = one[0] * two[0] + one[1] * two[2]
    db = one[0] * two[1] + one[1] * two[3]
    dc = one[2] * two[0] + one[3] * two[2]
    dd = one[2] * two[1] + one[3] * two[3]
    de = one[4] * two[0] + one[5] * two[2] + two[4]
    df = one[4] * two[1] + one[5] * two[3] + two[5]
    return (da, db, dc, dd, de, df)


def fmt(m):
    return "[" + " ".join(f"{v:.4f}" for v in m) + "]"


fsub1_idx = text.find("/Fsub1")
if fsub1_idx < 0:
    raise SystemExit("no /Fsub1 found")

# Tokenize from byte 0 to /Fsub1 looking for cm and q/Q
prelude = text[:fsub1_idx]
ctm = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)  # identity
stack = []
in_text = False
ops_seen = []

# Simple regex tokenization: numbers and word ops
token_re = re.compile(r"-?\d+\.?\d*(?:e[+-]?\d+)?|/\w+|\([^)]*\)|<[0-9a-fA-F]*>|[a-zA-Z*'\"]+")
operand_buf = []
for m in token_re.finditer(prelude):
    t = m.group()
    if t in ("BT",):
        in_text = True
        operand_buf = []
        continue
    if t in ("ET",):
        in_text = False
        operand_buf = []
        continue
    if in_text:
        # Text-state ops are inside BT...ET; they don't change CTM.
        operand_buf = []
        continue
    if t == "q":
        stack.append(ctm)
        operand_buf = []
        ops_seen.append(("q", None, ctm))
        continue
    if t == "Q":
        if stack:
            ctm = stack.pop()
        operand_buf = []
        ops_seen.append(("Q", None, ctm))
        continue
    if t == "cm":
        if len(operand_buf) >= 6:
            cm = tuple(float(x) for x in operand_buf[-6:])
            new_ctm = mul(cm, ctm)  # cm is applied as new transform pre-existing CTM:
            # In PDF: cm operator pre-multiplies new transform onto CTM such that
            # new_CTM(p) = cm(old_CTM(p))?  Actually PDF spec: cm a b c d e f =>
            # CTM := matrix(a,b,c,d,e,f) * CTM   (in column-vector terms).
            # In MuPDF's fz_concat sense: new_CTM = fz_concat(cm, old_CTM)
            # = "apply cm first, then old_CTM"
            # which in column-vector multiplication: old_CTM_M * cm_M * v
            # That is, glyph point v is first transformed by cm, then by old_CTM.
            new_ctm = mul(cm, ctm)
            ops_seen.append(("cm", cm, new_ctm))
            ctm = new_ctm
        operand_buf = []
        continue
    if t in ("gs", "scn", "SCN", "RG", "rg", "g", "G", "cs", "CS", "w", "J", "j", "d",
             "m", "l", "h", "f", "f*", "F", "S", "B", "B*", "b", "b*", "n", "re",
             "W", "W*", "Do", "Tf", "Tj", "TJ", "Td", "TD", "Tm", "T*", "Tc", "Tw",
             "Tr", "Tz", "TL", "Ts", "y", "v", "c", "i"):
        # Non-CTM operators
        operand_buf = []
        continue
    # Otherwise, treat as operand (number)
    operand_buf.append(t)


# Print the last ~30 ops leading up to /Fsub1
print(f"Total CTM-affecting ops: {len(ops_seen)}")
print("Last 40:")
for op, cm, ctm_after in ops_seen[-40:]:
    if cm is not None:
        print(f"  cm {fmt(cm)}  ->  CTM = {fmt(ctm_after)}")
    else:
        print(f"  {op}                                          CTM = {fmt(ctm_after)}")

print()
print(f"FINAL CTM at /Fsub1 BT injection point: {fmt(ctm)}")
print()
print("For comparison, the textFilter log captured:")
print("  ctm = [0.48, 0, 0, -0.48, 0, 350.68]")
