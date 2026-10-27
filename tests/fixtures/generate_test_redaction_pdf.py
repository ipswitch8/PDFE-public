# -*- coding: utf-8 -*-
"""
Generate test_redaction.pdf - a multi-page PDF with text, images, and line art.

Used for testing redaction mark creation, application, and verification.

Usage:
    python generate_test_redaction_pdf.py

Outputs:
    test_redaction.pdf in the same directory as this script.
"""

import os
import zlib
from PIL import Image, ImageDraw, ImageFont


def create_image_data(width: int, height: int) -> bytes:
    """Create a simple colored rectangle image (RGB raw bytes)."""
    img = Image.new("RGB", (width, height), (200, 220, 240))
    draw = ImageDraw.Draw(img)
    # Draw a red rectangle
    draw.rectangle([10, 10, width - 10, height - 10], outline=(255, 0, 0), width=3)
    # Draw some text in the image
    try:
        font = ImageFont.truetype("arial.ttf", 14)
    except (IOError, OSError):
        font = ImageFont.load_default()
    draw.text((20, 20), "Image Content", fill=(0, 0, 0), font=font)
    return img.tobytes("raw", "RGB")


def build_pdf() -> bytes:
    """Build a 3-page PDF with text, image, and line art for redaction testing."""
    objects = []

    # Object 1: Catalog
    obj1 = b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    objects.append((1, obj1))

    # Object 2: Pages (3 kids)
    obj2 = b"2 0 obj\n<< /Type /Pages /Kids [3 0 R 6 0 R 9 0 R] /Count 3 >>\nendobj\n"
    objects.append((2, obj2))

    # --- Page 1: Text content ---
    # Object 3: Page 1
    obj3 = (
        "3 0 obj\n"
        "<< /Type /Page /Parent 2 0 R\n"
        "   /MediaBox [0 0 612.0 792.0]\n"
        "   /Contents 4 0 R\n"
        "   /Resources << /Font << /F1 5 0 R >> >> >>\n"
        "endobj\n"
    ).encode()
    objects.append((3, obj3))

    # Object 4: Content stream for page 1 (text)
    # Page-1 text layout: confidential/redactable lines stay in the top
    # ~30 % of the page (canvas y_frac 0.08..0.32); the "remain visible"
    # paragraph is pushed down to canvas y_frac > 0.65 so that AC5
    # (test_unmarked_text_still_present_after_save_reopen) can draw a
    # top-half redaction mark without intersecting the bottom paragraph.
    # See P-G-BUG-1 triage in pipeline f2a8c3e1: prior layout had all
    # 6 lines crammed into the top 32 %, making the AC5 drag y_frac
    # 0.04..0.45 cover the entire text block and over-redact by design.
    page1_content = (
        "BT\n"
        "/F1 24 Tf\n"
        "100 700 Td\n"
        "(Confidential Document) Tj\n"
        "0 -40 Td\n"
        "/F1 14 Tf\n"
        "(This is sensitive text that should be redacted.) Tj\n"
        "0 -30 Td\n"
        "(Account Number: 1234-5678-9012) Tj\n"
        "0 -30 Td\n"
        "(Social Security: 123-45-6789) Tj\n"
        "0 -320 Td\n"
        "(This line should remain visible after redaction.) Tj\n"
        "0 -30 Td\n"
        "(Another line of public information here.) Tj\n"
        "ET\n"
    ).encode()
    page1_compressed = zlib.compress(page1_content, 6)
    obj4 = (
        f"4 0 obj\n"
        f"<< /Length {len(page1_compressed)} /Filter /FlateDecode >>\n"
        f"stream\n"
    ).encode() + page1_compressed + b"\nendstream\nendobj\n"
    objects.append((4, obj4))

    # Object 5: Font (Helvetica)
    obj5 = (
        "5 0 obj\n"
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\n"
        "endobj\n"
    ).encode()
    objects.append((5, obj5))

    # --- Page 2: Image content ---
    # Object 6: Page 2
    obj6 = (
        "6 0 obj\n"
        "<< /Type /Page /Parent 2 0 R\n"
        "   /MediaBox [0 0 612.0 792.0]\n"
        "   /Contents 7 0 R\n"
        "   /Resources << /XObject << /Im0 8 0 R >> /Font << /F1 5 0 R >> >> >>\n"
        "endobj\n"
    ).encode()
    objects.append((6, obj6))

    # Object 7: Content stream for page 2 (image + text)
    page2_content = (
        "q 200 0 0 150 100 500 cm /Im0 Do Q\n"
        "BT /F1 14 Tf 100 450 Td (Caption: Secret Photo) Tj ET\n"
    ).encode()
    page2_compressed = zlib.compress(page2_content, 6)
    obj7 = (
        f"7 0 obj\n"
        f"<< /Length {len(page2_compressed)} /Filter /FlateDecode >>\n"
        f"stream\n"
    ).encode() + page2_compressed + b"\nendstream\nendobj\n"
    objects.append((7, obj7))

    # Object 8: Image XObject (small 100x75 image)
    img_w, img_h = 100, 75
    img_data = create_image_data(img_w, img_h)
    img_compressed = zlib.compress(img_data, 6)
    obj8_header = (
        f"8 0 obj\n"
        f"<< /Type /XObject /Subtype /Image\n"
        f"   /Width {img_w} /Height {img_h}\n"
        f"   /ColorSpace /DeviceRGB /BitsPerComponent 8\n"
        f"   /Filter /FlateDecode\n"
        f"   /Length {len(img_compressed)} >>\n"
        f"stream\n"
    ).encode()
    obj8 = obj8_header + img_compressed + b"\nendstream\nendobj\n"
    objects.append((8, obj8))

    # --- Page 3: Line art ---
    # Object 9: Page 3
    obj9 = (
        "9 0 obj\n"
        "<< /Type /Page /Parent 2 0 R\n"
        "   /MediaBox [0 0 612.0 792.0]\n"
        "   /Contents 10 0 R\n"
        "   /Resources << /Font << /F1 5 0 R >> >> >>\n"
        "endobj\n"
    ).encode()
    objects.append((9, obj9))

    # Object 10: Content stream for page 3 (line art + text)
    page3_content = (
        "% Line art: rectangle\n"
        "1 0 0 RG\n"
        "2 w\n"
        "100 600 400 150 re S\n"
        "% Line art: diagonal line\n"
        "0 0 1 RG\n"
        "100 600 m 500 750 l S\n"
        "% Text\n"
        "BT /F1 14 Tf 150 650 Td (Text inside line art) Tj ET\n"
        "BT /F1 14 Tf 100 400 Td (Text below line art) Tj ET\n"
    ).encode()
    page3_compressed = zlib.compress(page3_content, 6)
    obj10 = (
        f"10 0 obj\n"
        f"<< /Length {len(page3_compressed)} /Filter /FlateDecode >>\n"
        f"stream\n"
    ).encode() + page3_compressed + b"\nendstream\nendobj\n"
    objects.append((10, obj10))

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
    print("Building redaction test PDF...")
    pdf_bytes = build_pdf()
    print(f"  PDF size: {len(pdf_bytes)} bytes")

    output_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "test_redaction.pdf")
    with open(output_path, "wb") as f:
        f.write(pdf_bytes)
    print(f"Written to: {output_path}")


if __name__ == "__main__":
    main()
