# -*- coding: utf-8 -*-
"""
Generate test_scanned.pdf - a scanned-style PDF with a full-page image XObject.

The image contains rendered text so Tesseract OCR can recognize it.
The image is embedded as a standard XObject with FlateDecode compression,
ensuring pdf_load_stream() can extract the raw pixel data.

Usage:
    python generate_test_scanned_pdf.py

Outputs:
    test_scanned.pdf in the same directory as this script.
"""

import struct
import zlib
import os
from PIL import Image, ImageDraw, ImageFont


def create_scanned_page_image(width: int, height: int) -> bytes:
    """Create a white page image with rendered text, simulating a scan."""
    img = Image.new("RGB", (width, height), (255, 255, 255))
    draw = ImageDraw.Draw(img)

    # Try to use a standard font; fall back to default
    font = None
    font_large = None
    for font_name in ["arial.ttf", "Arial.ttf", "DejaVuSans.ttf",
                       "LiberationSans-Regular.ttf", "FreeSans.ttf"]:
        try:
            font = ImageFont.truetype(font_name, 36)
            font_large = ImageFont.truetype(font_name, 48)
            break
        except (IOError, OSError):
            continue

    if font is None:
        font = ImageFont.load_default()
        font_large = font

    # Render text lines (simulating a scanned document)
    lines = [
        (100, 120, font_large, "Test Scanned Document"),
        (100, 200, font, "This is a test page for the PDFE"),
        (100, 250, font, "Text Reconstruction Engine."),
        (100, 330, font, "The quick brown fox jumps over"),
        (100, 380, font, "the lazy dog. Pack my box with"),
        (100, 430, font, "five dozen liquor jugs."),
        (100, 530, font, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"),
        (100, 580, font, "abcdefghijklmnopqrstuvwxyz"),
        (100, 630, font, "0123456789 !@#$%^&*()"),
    ]

    for x, y, f, text in lines:
        draw.text((x, y), text, fill=(0, 0, 0), font=f)

    # Convert to raw RGB bytes
    return img.tobytes("raw", "RGB")


def build_pdf(image_width: int, image_height: int, rgb_data: bytes,
              page_width: float = 612.0, page_height: float = 792.0) -> bytes:
    """Build a minimal PDF with the image as a FlateDecode XObject."""
    # Compress image data with zlib (FlateDecode)
    compressed = zlib.compress(rgb_data, 6)

    # PDF objects - we'll build them manually for full control
    objects = []  # list of (obj_num, bytes)

    # Object 1: Catalog
    obj1 = b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    objects.append((1, obj1))

    # Object 2: Pages
    obj2 = b"2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    objects.append((2, obj2))

    # Object 3: Page
    obj3 = (
        f"3 0 obj\n"
        f"<< /Type /Page /Parent 2 0 R\n"
        f"   /MediaBox [0 0 {page_width:.1f} {page_height:.1f}]\n"
        f"   /Contents 4 0 R\n"
        f"   /Resources << /XObject << /Im0 5 0 R >> >> >>\n"
        f"endobj\n"
    ).encode()
    objects.append((3, obj3))

    # Object 4: Content stream (draws the image full-page)
    content = f"q {page_width:.1f} 0 0 {page_height:.1f} 0 0 cm /Im0 Do Q".encode()
    content_compressed = zlib.compress(content, 6)
    obj4 = (
        f"4 0 obj\n"
        f"<< /Length {len(content_compressed)} /Filter /FlateDecode >>\n"
        f"stream\n"
    ).encode() + content_compressed + b"\nendstream\nendobj\n"
    objects.append((4, obj4))

    # Object 5: Image XObject
    obj5_header = (
        f"5 0 obj\n"
        f"<< /Type /XObject /Subtype /Image\n"
        f"   /Width {image_width} /Height {image_height}\n"
        f"   /ColorSpace /DeviceRGB /BitsPerComponent 8\n"
        f"   /Filter /FlateDecode\n"
        f"   /Length {len(compressed)} >>\n"
        f"stream\n"
    ).encode()
    obj5 = obj5_header + compressed + b"\nendstream\nendobj\n"
    objects.append((5, obj5))

    # Assemble PDF
    pdf = bytearray()
    pdf.extend(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")  # header + binary comment

    # Write objects and record offsets
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
    # 300 DPI for letter-size page: 2550x3300 pixels
    # Use a smaller size for faster tests: 850x1100 (100 DPI equivalent)
    image_width = 850
    image_height = 1100

    print(f"Generating {image_width}x{image_height} RGB image with text...")
    rgb_data = create_scanned_page_image(image_width, image_height)
    print(f"  Raw image size: {len(rgb_data)} bytes")

    print("Building PDF...")
    pdf_bytes = build_pdf(image_width, image_height, rgb_data)
    print(f"  PDF size: {len(pdf_bytes)} bytes")

    output_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "test_scanned.pdf")
    with open(output_path, "wb") as f:
        f.write(pdf_bytes)
    print(f"Written to: {output_path}")


if __name__ == "__main__":
    main()
