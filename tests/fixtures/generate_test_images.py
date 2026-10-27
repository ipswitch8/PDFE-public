#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate test fixtures for Phase 9 image operations.

Creates:
- test_image_rgb.png: Small 100x80 RGB PNG for insert tests
- test_pdf_with_images.pdf: PDF with 2 embedded images on page 1
- test_image_with_exif.jpg: JPEG with EXIF metadata for strip tests
"""

import os
import struct
import sys

def write_png(path, width, height, pixels):
    """Write a minimal PNG file."""
    import zlib

    def chunk(chunk_type, data):
        c = chunk_type + data
        crc = struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)
        return struct.pack('>I', len(data)) + c + crc

    # IHDR
    ihdr_data = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    ihdr = chunk(b'IHDR', ihdr_data)

    # IDAT
    raw_data = b''
    for y in range(height):
        raw_data += b'\x00'  # filter none
        for x in range(width):
            idx = (y * width + x) * 3
            raw_data += bytes(pixels[idx:idx+3])
    compressed = zlib.compress(raw_data)
    idat = chunk(b'IDAT', compressed)

    # IEND
    iend = chunk(b'IEND', b'')

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(ihdr)
        f.write(idat)
        f.write(iend)

def create_test_png(path):
    """Create a 100x80 RGB test image with a red-blue gradient."""
    width, height = 100, 80
    pixels = []
    for y in range(height):
        for x in range(width):
            r = int(255 * x / width)
            g = 100
            b = int(255 * y / height)
            pixels.extend([r, g, b])
    write_png(path, width, height, pixels)
    print(f"Created {path} ({width}x{height} RGB)")

def create_test_jpeg_with_exif(path):
    """Create a small JPEG with EXIF data.

    We create a minimal valid JPEG with an APP1 (EXIF) marker.
    """
    # Minimal 8x8 JPEG (grayscale, all gray)
    # SOI
    data = b'\xFF\xD8'

    # APP1 (EXIF) marker with some fake EXIF data
    exif_header = b'Exif\x00\x00'
    # TIFF header (little-endian)
    tiff_header = b'II'  # little-endian
    tiff_header += struct.pack('<H', 42)  # magic
    tiff_header += struct.pack('<I', 8)   # offset to first IFD
    # IFD with one entry: ImageDescription
    ifd = struct.pack('<H', 1)  # 1 entry
    # Tag 0x010E (ImageDescription), Type 2 (ASCII), Count 12, Value offset
    ifd += struct.pack('<HHI', 0x010E, 2, 12)
    ifd += b'Test EXIF\x00\x00\x00'  # inline value (fits in 4 bytes? no, 12 bytes)
    # Actually for >4 bytes we need an offset. Let's use a simpler tag.
    # Let's just put the EXIF marker with the header + minimal IFD
    ifd_data = struct.pack('<H', 1)  # 1 entry
    ifd_data += struct.pack('<HHII', 0x0112, 3, 1, 1)  # Orientation = 1
    ifd_data += struct.pack('<I', 0)  # next IFD = 0

    exif_data = exif_header + tiff_header + ifd_data
    app1_length = len(exif_data) + 2
    data += b'\xFF\xE1' + struct.pack('>H', app1_length) + exif_data

    # APP0 (JFIF) - needed for valid JPEG
    jfif = b'JFIF\x00\x01\x01\x00\x00\x48\x00\x48\x00\x00'
    data += b'\xFF\xE0' + struct.pack('>H', len(jfif) + 2) + jfif

    # DQT (quantization table)
    qt = bytes([8] * 64)  # simple uniform quantization
    data += b'\xFF\xDB' + struct.pack('>H', 67) + b'\x00' + qt

    # SOF0 (Start of Frame)
    sof = struct.pack('>BHHB', 8, 8, 8, 1)  # 8-bit, 8x8, 1 component
    sof += struct.pack('>BBB', 1, 0x11, 0)  # component 1, 1x1 sampling, quant table 0
    data += b'\xFF\xC0' + struct.pack('>H', len(sof) + 2) + sof

    # DHT (Huffman table - DC)
    # Minimal DC table
    dc_bits = bytes([0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
    dc_vals = bytes([0])
    dht_dc = b'\x00' + dc_bits + dc_vals
    data += b'\xFF\xC4' + struct.pack('>H', len(dht_dc) + 2) + dht_dc

    # DHT (Huffman table - AC)
    ac_bits = bytes([0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
    ac_vals = bytes([0])
    dht_ac = b'\x10' + ac_bits + ac_vals
    data += b'\xFF\xC4' + struct.pack('>H', len(dht_ac) + 2) + dht_ac

    # SOS (Start of Scan)
    sos = struct.pack('>B', 1)  # 1 component
    sos += struct.pack('>BB', 1, 0x00)  # component 1, DC table 0, AC table 0
    sos += struct.pack('>BBB', 0, 63, 0)  # spectral selection
    data += b'\xFF\xDA' + struct.pack('>H', len(sos) + 2) + sos

    # Entropy-coded data (minimal - single MCU of zeros)
    data += b'\xFB\x52\x80'  # minimal encoded data

    # EOI
    data += b'\xFF\xD9'

    with open(path, 'wb') as f:
        f.write(data)
    print(f"Created {path} (8x8 JPEG with EXIF)")

def create_pdf_with_images(path):
    """Create a PDF with 2 embedded images using raw PDF syntax."""
    # Image 1: 4x4 red square (RGB)
    img1_data = bytes([255, 0, 0] * 16)  # 4x4 red pixels

    # Image 2: 3x3 blue square (RGB)
    img2_data = bytes([0, 0, 255] * 9)  # 3x3 blue pixels

    objects = []

    # Object 1: Catalog
    objects.append(b'1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n')

    # Object 2: Pages
    objects.append(b'2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n')

    # Object 3: Page
    objects.append(
        b'3 0 obj\n<< /Type /Page /Parent 2 0 R '
        b'/MediaBox [0 0 612 792] '
        b'/Contents 4 0 R '
        b'/Resources << /XObject << /Im0 5 0 R /Im1 6 0 R >> >> '
        b'>>\nendobj\n'
    )

    # Object 4: Content stream (places both images)
    content = (
        b'q 200 0 0 200 50 550 cm /Im0 Do Q\n'
        b'q 150 0 0 150 300 400 cm /Im1 Do Q\n'
    )
    objects.append(
        b'4 0 obj\n<< /Length ' + str(len(content)).encode() + b' >>\n'
        b'stream\n' + content + b'endstream\nendobj\n'
    )

    # Object 5: Image 1 (4x4 red)
    objects.append(
        b'5 0 obj\n<< /Type /XObject /Subtype /Image '
        b'/Width 4 /Height 4 /ColorSpace /DeviceRGB '
        b'/BitsPerComponent 8 /Length ' + str(len(img1_data)).encode() + b' >>\n'
        b'stream\n' + img1_data + b'\nendstream\nendobj\n'
    )

    # Object 6: Image 2 (3x3 blue)
    objects.append(
        b'6 0 obj\n<< /Type /XObject /Subtype /Image '
        b'/Width 3 /Height 3 /ColorSpace /DeviceRGB '
        b'/BitsPerComponent 8 /Length ' + str(len(img2_data)).encode() + b' >>\n'
        b'stream\n' + img2_data + b'\nendstream\nendobj\n'
    )

    # Build the PDF
    pdf = b'%PDF-1.4\n'
    offsets = []
    for obj in objects:
        offsets.append(len(pdf))
        pdf += obj

    # Cross-reference table
    xref_offset = len(pdf)
    pdf += b'xref\n'
    pdf += f'0 {len(objects) + 1}\n'.encode()
    pdf += b'0000000000 65535 f \n'
    for offset in offsets:
        pdf += f'{offset:010d} 00000 n \n'.encode()

    # Trailer
    pdf += b'trailer\n'
    pdf += f'<< /Size {len(objects) + 1} /Root 1 0 R >>\n'.encode()
    pdf += b'startxref\n'
    pdf += f'{xref_offset}\n'.encode()
    pdf += b'%%EOF\n'

    with open(path, 'wb') as f:
        f.write(pdf)
    print(f"Created {path} (1 page, 2 images)")

if __name__ == '__main__':
    script_dir = os.path.dirname(os.path.abspath(__file__))

    create_test_png(os.path.join(script_dir, 'test_image_rgb.png'))
    create_pdf_with_images(os.path.join(script_dir, 'test_pdf_with_images.pdf'))
    create_test_jpeg_with_exif(os.path.join(script_dir, 'test_image_with_exif.jpg'))

    print("\nAll fixtures generated successfully.")
