# -*- coding: utf-8 -*-
"""Compare every page's content stream between the original Lean Startup
PDF and the user's saved 'Edited - ...' file. Report which page actually
changed (= the page that received the user's edit) and dump that page's
new content stream so we can inspect the rewrite.
"""
from __future__ import annotations

import hashlib
import pathlib
import sys

import pikepdf

ORIG = pathlib.Path(r"C:\Users\dev\Desktop\The Lean Startup - Erick Ries.pdf")
EDITED = pathlib.Path(r"C:\Users\dev\Desktop\Edited - The Lean Startup - Erick Ries.pdf")
OUT_DIR = pathlib.Path(__file__).parent


def page_content_bytes(page) -> bytes:
    contents = page.obj.get("/Contents", None)
    if contents is None:
        return b""
    streams = contents if isinstance(contents, pikepdf.Array) else [contents]
    out = b""
    for s in streams:
        out += s.read_bytes()
    return out


def main() -> int:
    if not ORIG.exists():
        print(f"FAIL: original not found at {ORIG}", file=sys.stderr)
        return 2
    if not EDITED.exists():
        print(f"FAIL: edited not found at {EDITED}", file=sys.stderr)
        return 2

    orig = pikepdf.open(ORIG)
    edited = pikepdf.open(EDITED)

    print(f"orig pages: {len(orig.pages)}  edited pages: {len(edited.pages)}")
    if len(orig.pages) != len(edited.pages):
        print("WARNING: page counts differ; alignment may be off")

    n = min(len(orig.pages), len(edited.pages))
    changed = []
    for i in range(n):
        ob = page_content_bytes(orig.pages[i])
        eb = page_content_bytes(edited.pages[i])
        oh = hashlib.sha1(ob).hexdigest()[:12]
        eh = hashlib.sha1(eb).hexdigest()[:12]
        same = oh == eh
        marker = "  same" if same else "  DIFF"
        print(f"  page {i + 1:>3}: orig={len(ob):>6}b  {oh}   edited={len(eb):>6}b  {eh}   {marker}")
        if not same:
            changed.append(i)

    print()
    print(f"changed pages (0-based index): {changed}")

    # Dump each changed page's edited content stream so we can read the
    # new BT...ET that ContentStreamEditor produced.
    for i in changed:
        eb = page_content_bytes(edited.pages[i])
        out = OUT_DIR / f"edited_page{i + 1}_content.bin"
        out.write_bytes(eb)
        print(f"  wrote {out.name} ({len(eb)} bytes)")

        # Also print the resources /Font dict for the changed page
        res = edited.pages[i].obj.get("/Resources", None)
        if res is not None:
            fonts = res.get("/Font", None)
            if fonts is not None:
                print(f"  page {i + 1} fonts: {sorted(fonts.keys())}")
                for k in fonts.keys():
                    f = fonts[k]
                    bf = f.get("/BaseFont", None)
                    st = f.get("/Subtype", None)
                    enc = f.get("/Encoding", None)
                    print(f"    {k}: BaseFont={bf}  Subtype={st}  Encoding={enc}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
