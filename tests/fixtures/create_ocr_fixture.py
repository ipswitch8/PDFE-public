# -*- coding: utf-8 -*-
"""
Create OCR Test Fixture PDF

This script generates a test PDF file that simulates an OCR'd document.
It contains a full-page image with invisible text overlay, similar to
what OCR software produces.

Usage:
    python create_ocr_fixture.py

Output:
    test_ocr_text.pdf - A PDF file with image background and OCR text layer
"""

import io
import os
import sys
from pathlib import Path

try:
    import fitz  # PyMuPDF
except ImportError:
    print("PyMuPDF not found. Installing pymupdf...")
    import subprocess
    subprocess.check_call([sys.executable, "-m", "pip", "install", "pymupdf"])
    import fitz

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("Pillow not found. Installing pillow...")
    import subprocess
    subprocess.check_call([sys.executable, "-m", "pip", "install", "pillow"])
    from PIL import Image, ImageDraw


def create_background_image(width=612, height=792):
    """
    Create a simple background image with gradient.

    Args:
        width: Image width in pixels (default: 612 for letter size)
        height: Image height in pixels (default: 792 for letter size)

    Returns:
        PIL.Image: Generated background image
    """
    # Create image with gradient background
    image = Image.new('RGB', (width, height), color='white')
    draw = ImageDraw.Draw(image)

    # Draw a simple gradient (light blue to white)
    for y in range(height):
        # Calculate color gradient
        intensity = int(255 - (y / height) * 30)  # Subtle gradient
        color = (intensity - 20, intensity - 10, intensity)
        draw.line([(0, y), (width, y)], fill=color)

    # Add some visual elements to make it look like a scanned document
    # Add a light texture pattern
    for x in range(0, width, 50):
        for y in range(0, height, 50):
            if (x + y) % 100 == 0:
                draw.ellipse([x, y, x + 5, y + 5], fill=(240, 240, 240))

    return image


def create_ocr_pdf(output_path, page_width=612, page_height=792):
    """
    Create a PDF with a full-page image and invisible text overlay.

    Args:
        output_path: Path where the PDF should be saved
        page_width: Page width in points (default: 612 for letter size)
        page_height: Page height in points (default: 792 for letter size)
    """
    # Create a new PDF document
    doc = fitz.open()

    # Add a page
    page = doc.new_page(width=page_width, height=page_height)

    # Create background image
    print("Creating background image...")
    img = create_background_image(width=int(page_width), height=int(page_height))

    # Convert PIL image to bytes
    img_buffer = io.BytesIO()
    img.save(img_buffer, format='PNG')
    img_bytes = img_buffer.getvalue()

    # Insert the image to cover the entire page
    print("Adding image to PDF...")
    img_rect = fitz.Rect(0, 0, page_width, page_height)
    page.insert_image(img_rect, stream=img_bytes)

    # Add invisible text overlay (simulating OCR text layer)
    # This text will be extractable but not visible
    print("Adding invisible OCR text layer...")

    # Sample text content that would be found by OCR
    ocr_text_content = """INVOICE

Invoice Number: INV-2024-001
Date: January 15, 2024

Bill To:
John Doe
123 Main Street
Anytown, ST 12345

Description                          Quantity    Price      Total
--------------------------------------------------------------------
Professional Services                    10      $150.00   $1,500.00
Consulting Hours                          5      $200.00   $1,000.00
Software License                          1      $500.00     $500.00

                                              Subtotal:   $3,000.00
                                              Tax (8%):     $240.00
                                              Total:      $3,240.00

Payment Terms: Net 30
Due Date: February 14, 2024

Thank you for your business!
"""

    # Insert text in multiple positions across the page
    # Use a very small, light-colored font to make it nearly invisible
    text_lines = ocr_text_content.strip().split('\n')
    y_position = 50
    line_height = 20

    for line in text_lines:
        if line.strip():
            # Insert text with white color (invisible on white background)
            # This simulates how OCR software adds a hidden text layer
            text_point = fitz.Point(50, y_position)

            # Add the text as invisible (render mode 3 = invisible)
            rc = page.insert_text(
                text_point,
                line,
                fontsize=11,
                color=(1, 1, 1),  # White text (invisible on white background)
                render_mode=3,    # Invisible text (but still extractable)
            )

            y_position += line_height

    # Also add some visible text at the top to indicate this is a test file
    header_point = fitz.Point(page_width / 2 - 100, 30)
    page.insert_text(
        header_point,
        "OCR Test Fixture",
        fontsize=8,
        color=(0.7, 0.7, 0.7),  # Light gray
    )

    # Save the PDF
    print(f"Saving PDF to {output_path}...")
    doc.save(output_path)
    doc.close()

    print(f"[OK] Successfully created OCR test PDF: {output_path}")

    # Verify the file was created
    if os.path.exists(output_path):
        file_size = os.path.getsize(output_path)
        print(f"  File size: {file_size:,} bytes")

        # Verify text is extractable
        verify_doc = fitz.open(output_path)
        verify_page = verify_doc[0]
        extracted_text = verify_page.get_text()
        verify_doc.close()

        if extracted_text.strip():
            print(f"  [OK] Text extraction verified ({len(extracted_text)} characters)")
            print(f"  Preview: {extracted_text[:100]}...")
        else:
            print("  [WARN] Warning: No text could be extracted")
    else:
        print(f"  [ERROR] Error: File was not created")


def main():
    """Main entry point."""
    # Determine output path
    script_dir = Path(__file__).parent
    output_file = script_dir / "test_ocr_text.pdf"

    print("=" * 70)
    print("OCR Test Fixture Generator")
    print("=" * 70)
    print(f"Output file: {output_file}")
    print()

    # Create the PDF
    create_ocr_pdf(str(output_file))

    print()
    print("=" * 70)
    print("Done!")
    print("=" * 70)


if __name__ == "__main__":
    main()
