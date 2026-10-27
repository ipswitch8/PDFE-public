# -*- coding: utf-8 -*-
"""
gen_nested_parent.py

Generates a minimal PDF with a 1000-level deep /Parent chain for fuzz
corpus use. The structure exercises MuPDF's pdf_load_page_tree depth guard
(or triggers stack overflow if none exists).

Object layout:
  obj 1:    Catalog -> /Pages 2 0 R
  obj 2:    /Pages root (/Kids [3 0 R], /Count 1, no /Parent)
  obj 3:    /Pages (/Kids [4 0 R], /Count 1, /Parent 2 0 R)
  ...
  obj 1001: /Pages (/Kids [1002 0 R], /Count 1, /Parent 1000 0 R)
  obj 1002: /Page (/Parent 1001 0 R, /MediaBox [0 0 612 792])

Total objects: 1002
"""

import os
import sys

DEPTH = 1000
OUTPUT_PATH = (
    r"C:\Users\dev\source\PDFE\tests\security\fuzz\seeds"
    r"\fuzz_document_open\nested_parent_1000.pdf"
)

# Total objects:
#   1  = Catalog
#   1  = Pages root (obj 2)
#   DEPTH-1 = intermediate Pages nodes (obj 3 .. obj DEPTH)
#   Wait — obj 2 is level 0 (root), obj 3..1001 are levels 1..999 (999 nodes),
#   obj 1002 is the leaf Page.
# Chain depth: 1000 /Parent hops from leaf back to root means
#   root=obj2, node3..node1001 (999 intermediate), leaf=obj1002
#   leaf->1001->...->2 = 1001 hops... let's be deliberate:
#   We want exactly 1000 levels of /Pages nodes (obj 2 through obj 1001),
#   then 1 leaf /Page (obj 1002).
#   That gives 1000 /Pages objects; the leaf's /Parent chain is 1000 hops deep.

TOTAL_OBJECTS = 1 + DEPTH + 1  # catalog + 1000 pages nodes + 1 leaf page
# = 1002

def build_pdf() -> bytes:
    parts: list[bytes] = []
    offsets: list[int] = []  # offsets[i] = byte offset of object (i+1) (1-based)

    def emit(data: bytes) -> None:
        parts.append(data)

    current_offset = [0]

    def record_offset() -> None:
        offsets.append(current_offset[0])

    def add(data: bytes) -> None:
        record_offset()
        emit(data)
        current_offset[0] += len(data)

    # ------------------------------------------------------------------ header
    header = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"
    emit(header)
    current_offset[0] += len(header)

    # ------------------------------------------------------------------ obj 1: Catalog
    obj1 = b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    add(obj1)

    # ------------------------------------------------------------------ obj 2: Pages root
    obj2 = b"2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    add(obj2)

    # ------------------------------------------------------------------ obj 3..1001: intermediate Pages nodes
    # obj N has /Parent (N-1) 0 R and /Kids [(N+1) 0 R]
    for n in range(3, DEPTH + 2):  # 3..1001 inclusive
        parent_ref = n - 1
        child_ref = n + 1
        body = (
            f"{n} 0 obj\n"
            f"<< /Type /Pages /Kids [{child_ref} 0 R] /Count 1 "
            f"/Parent {parent_ref} 0 R >>\n"
            f"endobj\n"
        ).encode("ascii")
        add(body)

    # ------------------------------------------------------------------ obj 1002: leaf Page
    leaf_parent = DEPTH + 1  # obj 1001
    leaf_n = DEPTH + 2       # obj 1002
    obj_leaf = (
        f"{leaf_n} 0 obj\n"
        f"<< /Type /Page /Parent {leaf_parent} 0 R "
        f"/MediaBox [0 0 612 792] >>\n"
        f"endobj\n"
    ).encode("ascii")
    add(obj_leaf)

    # ------------------------------------------------------------------ xref
    xref_offset = current_offset[0]
    assert len(offsets) == TOTAL_OBJECTS, (
        f"Expected {TOTAL_OBJECTS} offsets, got {len(offsets)}"
    )

    xref_lines = [f"xref\n0 {TOTAL_OBJECTS + 1}\n".encode("ascii")]
    # free entry for obj 0
    xref_lines.append(b"0000000000 65535 f \n")
    for off in offsets:
        xref_lines.append(f"{off:010d} 00000 n \n".encode("ascii"))

    emit(b"".join(xref_lines))

    # ------------------------------------------------------------------ trailer
    trailer = (
        f"trailer\n"
        f"<< /Size {TOTAL_OBJECTS + 1} /Root 1 0 R >>\n"
        f"startxref\n"
        f"{xref_offset}\n"
        f"%%EOF\n"
    ).encode("ascii")
    emit(trailer)

    return b"".join(parts)


def main() -> None:
    pdf_bytes = build_pdf()

    os.makedirs(os.path.dirname(OUTPUT_PATH), exist_ok=True)
    with open(OUTPUT_PATH, "wb") as fh:
        fh.write(pdf_bytes)

    size = os.path.getsize(OUTPUT_PATH)
    print(f"Written: {OUTPUT_PATH}")
    print(f"File size: {size:,} bytes ({size / 1024:.1f} KB)")
    print(f"Objects: {TOTAL_OBJECTS} (1 Catalog + {DEPTH} /Pages chain + 1 leaf /Page)")
    print(f"/Parent chain depth: {DEPTH}")


if __name__ == "__main__":
    main()
