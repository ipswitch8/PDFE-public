# -*- coding: utf-8 -*-
"""Find the rewritten BT...ET on page 3 of the edited PDF and dump it
in human-readable form so we can see whether ContentStreamEditor's Tm,
Tf, Tj match the log values (or diverge from them).
"""
from __future__ import annotations

import pathlib
import re

DATA = pathlib.Path(__file__).parent / "edited_page3_content.bin"
raw = DATA.read_bytes()
text = raw.decode("latin-1")  # preserve byte values

# Find all /Fsub1 occurrences
for m in re.finditer(r"/Fsub1[^\n]*\n", text):
    start = max(0, m.start() - 200)
    end = min(len(text), m.end() + 800)
    print("=" * 70)
    print(f"OCCURRENCE at byte {m.start()}")
    print("=" * 70)
    snippet = text[start:end]
    # Render hex of any low-byte sequences after Tj <hex...>
    print(snippet)
    print()

# Also locate every BT/ET block boundary
print("=" * 70)
print("ALL BT...ET blocks (with sizes)")
print("=" * 70)
i = 0
n = 0
while True:
    bt = text.find("BT", i)
    if bt < 0:
        break
    et = text.find("ET", bt)
    if et < 0:
        break
    block = text[bt : et + 2]
    n += 1
    print(f"  block {n}: bytes {bt}..{et + 2} ({et + 2 - bt} bytes)")
    # Print first font reference and any Tj
    f_match = re.search(r"/(F\d+|Fsub\d+)\s+([\d.]+)\s+Tf", block)
    tm_match = re.search(r"([\d.\-eE]+)\s+([\d.\-eE]+)\s+([\d.\-eE]+)\s+([\d.\-eE]+)\s+([\d.\-eE]+)\s+([\d.\-eE]+)\s+Tm", block)
    if f_match:
        print(f"    Tf: /{f_match.group(1)} {f_match.group(2)}")
    if tm_match:
        print(f"    Tm: [{', '.join(tm_match.groups())}]")
    # Count Tj occurrences and print first one
    tjs = re.findall(r"<([0-9a-fA-F]+)>\s*Tj", block)
    print(f"    Tj count: {len(tjs)}, first: {tjs[0] if tjs else 'none'}")
    i = et + 2

# Specifically look for the long Tj that should contain "Eric Ries unxravels..."
print()
print("=" * 70)
print("Long Tj's (>= 32 hex chars):")
print("=" * 70)
for m in re.finditer(r"<([0-9a-fA-F]+)>\s*Tj", text):
    hex_str = m.group(1)
    if len(hex_str) >= 32:
        # Decode 2-byte CIDs and show codepoints
        cids = [int(hex_str[i : i + 4], 16) for i in range(0, len(hex_str), 4)]
        print(f"  byte offset {m.start()}: {len(hex_str) // 4} CIDs = {cids[:20]}{'...' if len(cids) > 20 else ''}")
