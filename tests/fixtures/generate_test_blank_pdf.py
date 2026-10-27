# -*- coding: utf-8 -*-
"""
Generate test_blank.pdf - a minimal blank PDF with a single empty page.

Used for testing graceful handling of pages with no image/text content.

Usage:
    python generate_test_blank_pdf.py

Outputs:
    test_blank.pdf in the same directory as this script.
"""

import os


def build_blank_pdf(page_width: float = 612.0, page_height: float = 792.0) -> bytes:
    """Build a minimal PDF with a single blank page (no content stream)."""
    objects = []

    # Object 1: Catalog
    obj1 = b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    objects.append((1, obj1))

    # Object 2: Pages
    obj2 = b"2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    objects.append((2, obj2))

    # Object 3: Page (blank - no Contents, no Resources)
    obj3 = (
        f"3 0 obj\n"
        f"<< /Type /Page /Parent 2 0 R\n"
        f"   /MediaBox [0 0 {page_width:.1f} {page_height:.1f}] >>\n"
        f"endobj\n"
    ).encode()
    objects.append((3, obj3))

    # Assemble PDF
    pdf = bytearray()
    pdf.extend(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")

    offsets = {}
    for obj_num, obj_bytes in objects:
        offsets[obj_num] = len(pdf)
        pdf.extend(obj_bytes)

    # Cross-reference table
    xref_offset = len(pdf)
    pdf.extend(b"xref\n")
    pdf.extend(f"0 {len(objects) + 1}\n".encode())
    pdf.extend(b"0000000000 65535 f \n")
    for obj_num in range(1, len(objects) + 1):
        pdf.extend(f"{offsets[obj_num]:010d} 00000 n \n".encode())

    # Trailer
    pdf.extend(b"trailer\n")
    pdf.extend(f"<< /Size {len(objects) + 1} /Root 1 0 R >>\n".encode())
    pdf.extend(b"startxref\n")
    pdf.extend(f"{xref_offset}\n".encode())
    pdf.extend(b"%%EOF\n")

    return bytes(pdf)


def main():
    print("Building blank PDF...")
    pdf_bytes = build_blank_pdf()
    print(f"  PDF size: {len(pdf_bytes)} bytes")

    output_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "test_blank.pdf")
    with open(output_path, "wb") as f:
        f.write(pdf_bytes)
    print(f"Written to: {output_path}")


if __name__ == "__main__":
    main()
