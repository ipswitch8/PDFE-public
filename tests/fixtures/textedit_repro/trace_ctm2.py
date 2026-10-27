# -*- coding: utf-8 -*-
"""Properly trace CTM in the saved page-3 content stream by handling
decimal numbers correctly (the previous regex was greedy on \\d+).

Goal: determine the actual CTM at the byte where ContentStreamEditor
emitted /Fsub1 BT. If that CTM differs from the [0.48,0,0,-0.48,0,350.68]
that the textFilter callback recorded for the original character, then
the math `trm * ctm_inv` was inverting against a stale CTM and the
emitted Tm has the wrong scale and/or sign for its actual location."""
from __future__ import annotations

import pathlib
import re

DATA = pathlib.Path(__file__).parent / "edited_page3_content.bin"
text = DATA.read_bytes().decode("latin-1")

NUM = re.compile(r"-?(?:\d+\.\d+|\d+|\.\d+)(?:[eE][+-]?\d+)?")


def fz_concat(one, two):
    """MuPDF's fz_concat(one, two): apply one then two."""
    a = one[0] * two[0] + one[1] * two[2]
    b = one[0] * two[1] + one[1] * two[3]
    c = one[2] * two[0] + one[3] * two[2]
    d = one[2] * two[1] + one[3] * two[3]
    e = one[4] * two[0] + one[5] * two[2] + two[4]
    f = one[4] * two[1] + one[5] * two[3] + two[5]
    return (a, b, c, d, e, f)


def fmt(m):
    return "[" + " ".join(f"{v:.4f}" for v in m) + "]"


fsub1 = text.find("/Fsub1")
prelude = text[:fsub1]
print(f"/Fsub1 at byte {fsub1}, scanning {len(prelude)} bytes of prelude")
print()

# Scan operators left-to-right. When we hit an operator (cm, q, Q, BT, ET),
# the immediately-preceding numbers are the operands.
ctm = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
stack = [ctm]
in_text = False
last_nums: list[float] = []

# Tokenize: keep track of position so we can find the BT byte offset
# We split on whitespace, but treat the operator words separately.
i = 0
events = []
while i < len(prelude):
    ch = prelude[i]
    if ch.isspace():
        i += 1
        continue
    # Try number
    m = NUM.match(prelude[i:])
    if m:
        m_end = i + len(m.group())
    else:
        m_end = i
    if m and (m_end == len(prelude) or not prelude[m_end].isalpha()):
        try:
            v = float(m.group())
            last_nums.append(v)
            i = m_end
            continue
        except ValueError:
            pass
    # Try operator word: a-zA-Z, possibly followed by '*'
    m = re.compile(r"[a-zA-Z][a-zA-Z0-9*]*").match(prelude[i:])
    if m:
        op = m.group()
        i += len(m.group())
        if op == "BT":
            in_text = True
            last_nums = []
            continue
        if op == "ET":
            in_text = False
            last_nums = []
            continue
        if in_text:
            last_nums = []
            continue
        if op == "q":
            stack.append(ctm)
            events.append(("q", None, ctm))
            last_nums = []
            continue
        if op == "Q":
            if len(stack) > 1:
                ctm = stack.pop()
            events.append(("Q", None, ctm))
            last_nums = []
            continue
        if op == "cm":
            if len(last_nums) >= 6:
                cm = tuple(last_nums[-6:])
                ctm = fz_concat(cm, ctm)
                events.append(("cm", cm, ctm))
            last_nums = []
            continue
        # Other ops: just consume operands
        last_nums = []
        continue
    # Skip /Name tokens, <hex> tokens, ()-strings, [arrays]
    if ch == "/":
        m2 = re.compile(r"/[^\s\[\]()<>/]+").match(prelude[i:])
        if m2:
            i += len(m2.group())
            continue
    if ch == "<":
        end = prelude.find(">", i)
        if end > 0:
            i = end + 1
            continue
    if ch == "(":
        # crude string-end finder
        depth = 1
        j = i + 1
        while j < len(prelude) and depth > 0:
            if prelude[j] == "\\" and j + 1 < len(prelude):
                j += 2
                continue
            if prelude[j] == "(":
                depth += 1
            elif prelude[j] == ")":
                depth -= 1
            j += 1
        i = j
        continue
    if ch == "[":
        end = prelude.find("]", i)
        if end > 0:
            i = end + 1
            continue
    i += 1

print(f"events seen: {len(events)}; final stack depth: {len(stack)}")
print()
print("First 30 events:")
for kind, payload, after in events[:30]:
    if kind == "cm":
        print(f"  cm{fmt(payload)}  ->  CTM = {fmt(after)}")
    else:
        print(f"  {kind}                                        CTM = {fmt(after)}")
print()
print("Last 10 events:")
for kind, payload, after in events[-10:]:
    if kind == "cm":
        print(f"  cm{fmt(payload)}  ->  CTM = {fmt(after)}")
    else:
        print(f"  {kind}                                        CTM = {fmt(after)}")

print()
print(f"FINAL CTM right before /Fsub1 BT: {fmt(ctm)}")
print()
print("textFilter logged ctm at the matched character:")
print("  [0.48, 0, 0, -0.48, 0, 350.68]")
