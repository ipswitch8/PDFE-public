# -*- coding: utf-8 -*-
"""
Generate encrypted PDF test fixtures using pikepdf.

This script creates three encrypted PDF files with different encryption configurations:
1. test_encrypted_aes256.pdf - Standard AES-256 encryption with all permissions
2. test_encrypted_restricted.pdf - AES-256 with restricted permissions (no print/copy/modify)
3. test_encrypted_owner_only.pdf - AES-256 with owner password only (no user password)

Author: Generated for PDFE test suite
Created: 2026-02-11
"""

import sys
import os
from pathlib import Path

# Ensure UTF-8 encoding for Windows
if sys.platform == 'win32':
    import codecs
    sys.stdout = codecs.getwriter('utf-8')(sys.stdout.buffer, 'strict')
    sys.stderr = codecs.getwriter('utf-8')(sys.stderr.buffer, 'strict')

import pikepdf
from pikepdf import Pdf, Dictionary, Name, Array, Stream


def create_base_pdf_with_text(text_lines: list) -> Pdf:
    """
    Create a simple PDF with text content using pikepdf's low-level API.

    Args:
        text_lines: List of text strings to display on the page

    Returns:
        Pdf object with a single page containing the text
    """
    pdf = Pdf.new()

    # Create a page (Letter size: 612 x 792 points)
    page_dict = Dictionary(
        Type=Name.Page,
        MediaBox=[0, 0, 612, 792],
        Contents=pdf.make_indirect(Stream(pdf, b"")),
        Resources=Dictionary(
            Font=Dictionary(
                F1=Dictionary(
                    Type=Name.Font,
                    Subtype=Name.Type1,
                    BaseFont=Name.Helvetica
                )
            )
        )
    )

    # Build content stream with text
    content_lines = [
        b"BT",  # Begin text
        b"/F1 12 Tf",  # Set font to Helvetica 12pt
        b"50 750 Td",  # Position at (50, 750)
    ]

    # Add each text line
    y_offset = 0
    for line in text_lines:
        if y_offset > 0:
            content_lines.append(f"0 -{y_offset} Td".encode('ascii'))
        # Escape special characters in PDF strings
        escaped_text = line.replace('\\', '\\\\').replace('(', '\\(').replace(')', '\\)')
        content_lines.append(f"({escaped_text}) Tj".encode('utf-8'))
        y_offset = 20  # Line spacing

    content_lines.append(b"ET")  # End text

    # Set the content stream
    content_stream = b"\n".join(content_lines)
    page_dict.Contents = Stream(pdf, content_stream)

    # Add page to PDF
    if "/Pages" not in pdf.Root:
        pdf.Root.Pages = pdf.make_indirect(Dictionary(
            Type=Name.Pages,
            Kids=[],
            Count=0
        ))

    page = pdf.make_indirect(page_dict)
    page.Parent = pdf.Root.Pages
    pdf.Root.Pages.Kids.append(page)
    pdf.Root.Pages.Count = len(pdf.Root.Pages.Kids)

    return pdf


def create_encrypted_aes256():
    """
    Create test_encrypted_aes256.pdf with standard AES-256 encryption.
    User password: "test123"
    Owner password: "owner456"
    All permissions allowed.
    """
    print("Creating test_encrypted_aes256.pdf...")

    text_lines = [
        "This is an encrypted test document",
        "Page 1 of test_encrypted_aes256.pdf",
        "",
        "Encryption: AES-256 (R6)",
        "User password: test123",
        "Owner password: owner456",
        "Permissions: All allowed"
    ]

    pdf = create_base_pdf_with_text(text_lines)

    # Save with AES-256 encryption, all permissions
    encryption = pikepdf.Encryption(
        owner="owner456",
        user="test123",
        R=6,  # Revision 6 (AES-256)
        allow=pikepdf.Permissions(
            accessibility=True,
            extract=True,
            modify_annotation=True,
            modify_assembly=True,
            modify_form=True,
            modify_other=True,
            print_lowres=True,
            print_highres=True
        )
    )

    output_path = Path(__file__).parent / "test_encrypted_aes256.pdf"
    pdf.save(output_path, encryption=encryption)
    print(f"✓ Created: {output_path}")
    print(f"  User password: test123")
    print(f"  Owner password: owner456")
    print(f"  Permissions: All allowed")
    print()


def create_encrypted_restricted():
    """
    Create test_encrypted_restricted.pdf with restricted permissions.
    User password: "test123"
    Owner password: "owner456"
    Permissions: NO print, NO copy, NO modify (but annotate/form fill/assemble/accessibility allowed).
    """
    print("Creating test_encrypted_restricted.pdf...")

    text_lines = [
        "This is a restricted document",
        "No printing, copying, or modifying allowed",
        "",
        "Encryption: AES-256 (R6)",
        "User password: test123",
        "Owner password: owner456",
        "Permissions: Restricted (no print/copy/modify)"
    ]

    pdf = create_base_pdf_with_text(text_lines)

    # Save with restricted permissions
    encryption = pikepdf.Encryption(
        owner="owner456",
        user="test123",
        R=6,
        allow=pikepdf.Permissions(
            accessibility=True,       # Allow accessibility
            extract=False,            # NO copying/extraction
            modify_annotation=True,   # Allow annotation
            modify_assembly=True,     # Allow assembly
            modify_form=True,         # Allow form filling
            modify_other=False,       # NO other modifications
            print_lowres=False,       # NO low-res printing
            print_highres=False       # NO high-res printing
        )
    )

    output_path = Path(__file__).parent / "test_encrypted_restricted.pdf"
    pdf.save(output_path, encryption=encryption)
    print(f"✓ Created: {output_path}")
    print(f"  User password: test123")
    print(f"  Owner password: owner456")
    print(f"  Permissions: NO print, NO copy, NO modify")
    print()


def create_encrypted_owner_only():
    """
    Create test_encrypted_owner_only.pdf with only owner password.
    No user password (empty string).
    Owner password: "owner456"
    All permissions allowed.
    """
    print("Creating test_encrypted_owner_only.pdf...")

    text_lines = [
        "Owner-only encrypted document",
        "No user password required to open",
        "",
        "Encryption: AES-256 (R6)",
        "User password: (none - empty string)",
        "Owner password: owner456",
        "Permissions: All allowed"
    ]

    pdf = create_base_pdf_with_text(text_lines)

    # Save with owner password only (empty user password)
    encryption = pikepdf.Encryption(
        owner="owner456",
        user="",  # Empty user password
        R=6,
        allow=pikepdf.Permissions(
            accessibility=True,
            extract=True,
            modify_annotation=True,
            modify_assembly=True,
            modify_form=True,
            modify_other=True,
            print_lowres=True,
            print_highres=True
        )
    )

    output_path = Path(__file__).parent / "test_encrypted_owner_only.pdf"
    pdf.save(output_path, encryption=encryption)
    print(f"✓ Created: {output_path}")
    print(f"  User password: (empty)")
    print(f"  Owner password: owner456")
    print(f"  Permissions: All allowed")
    print()


def main():
    """Generate all encrypted PDF test fixtures."""
    print("=" * 70)
    print("Generating Encrypted PDF Test Fixtures")
    print("=" * 70)
    print()

    try:
        create_encrypted_aes256()
        create_encrypted_restricted()
        create_encrypted_owner_only()

        print("=" * 70)
        print("✓ All encrypted PDF fixtures generated successfully!")
        print("=" * 70)
        print()
        print("Summary:")
        print("  1. test_encrypted_aes256.pdf - AES-256, all permissions")
        print("  2. test_encrypted_restricted.pdf - AES-256, restricted permissions")
        print("  3. test_encrypted_owner_only.pdf - AES-256, owner password only")
        print()

        # Verify files exist
        fixtures_dir = Path(__file__).parent
        for filename in ["test_encrypted_aes256.pdf", "test_encrypted_restricted.pdf",
                        "test_encrypted_owner_only.pdf"]:
            filepath = fixtures_dir / filename
            if filepath.exists():
                size = filepath.stat().st_size
                print(f"  ✓ {filename} ({size:,} bytes)")
            else:
                print(f"  ✗ {filename} (NOT FOUND)")

        return 0

    except Exception as e:
        print(f"✗ Error generating PDFs: {e}")
        import traceback
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    sys.exit(main())
