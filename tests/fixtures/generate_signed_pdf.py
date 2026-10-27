# -*- coding: utf-8 -*-
"""
Generate test PFX certificate and signed/unsigned PDFs for signature tests.

Outputs:
  - test_signer.pfx (legacy PFX from earlier phases; CN "PDFE Test Signer",
    password "test123") — kept for backwards compatibility.
  - test_unsigned_sigfield.pdf — single-page PDF with an empty signature
    field; signature dialog tests open this fixture and sign it live.
  - test_signed.pdf — a REAL PKCS#7-signed PDF.  Built in two steps:
      (1) fpdf2 emits the page,
      (2) pyhanko adds a signature field and signs it with the committed
          phase-12 PFX (tests/fixtures/certs/phase12_test_signer.pfx,
          password "testpw123").
    The resulting file has a valid /ByteRange + valid PKCS#7 /Contents.

Phase-12 remediation note (pipeline a7c2e541): historically
test_signed.pdf was an UNSIGNED placeholder with only the /Sig FIELD
present (no /ByteRange, no /Contents).  Phase-12 AC4/AC5 require an
actually-signed fixture so tamper-detection tests can fail the hash
check rather than falling through to "Unsigned" status.

Requires:  pip install cryptography fpdf2 pyhanko
"""

import sys
import os
import datetime
from pathlib import Path


def generate_pfx(output_path: Path, password: str = "test123"):
    """Generate a self-signed PFX certificate for testing (legacy)."""
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.serialization import pkcs12
    from cryptography.hazmat.primitives.asymmetric import rsa

    # Generate RSA key pair
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)

    # Build self-signed certificate
    subject = issuer = x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, "US"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "PDFE Test"),
        x509.NameAttribute(NameOID.ORGANIZATIONAL_UNIT_NAME, "QA"),
        x509.NameAttribute(NameOID.COMMON_NAME, "PDFE Test Signer"),
        x509.NameAttribute(NameOID.EMAIL_ADDRESS, "test@pdfe.example.com"),
    ])

    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.datetime(2024, 1, 1, tzinfo=datetime.timezone.utc))
        .not_valid_after(datetime.datetime(2030, 12, 31, tzinfo=datetime.timezone.utc))
        .add_extension(
            x509.BasicConstraints(ca=False, path_length=None), critical=True
        )
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                content_commitment=True,  # non_repudiation
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=False,
                crl_sign=False,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .sign(key, hashes.SHA256())
    )

    # Export as PKCS12 / PFX
    pfx_data = pkcs12.serialize_key_and_certificates(
        name=b"PDFE Test Signer",
        key=key,
        cert=cert,
        cas=None,
        encryption_algorithm=serialization.BestAvailableEncryption(password.encode("utf-8")),
    )

    output_path.write_bytes(pfx_data)
    print(f"Generated PFX: {output_path} (password: {password})")
    return cert, key


def generate_unsigned_sigfield_pdf(output_path: Path):
    """Generate a PDF with an empty (unsigned) signature field."""
    from fpdf import FPDF

    pdf = FPDF()
    pdf.add_page()
    pdf.set_font("Helvetica", size=14)
    pdf.cell(0, 10, "Document with Signature Field", new_x="LMARGIN", new_y="NEXT")
    pdf.set_font("Helvetica", size=10)
    pdf.cell(0, 10, "This document contains an empty signature field for testing.", new_x="LMARGIN", new_y="NEXT")
    pdf.ln(20)
    pdf.cell(0, 10, "Signed: _________________________", new_x="LMARGIN", new_y="NEXT")

    pdf.output(str(output_path))
    print(f"Generated unsigned PDF: {output_path}")


def generate_real_signed_pdf(unsigned_path: Path, output_path: Path,
                              pfx_path: Path, pfx_password: str):
    """Sign ``unsigned_path`` with the committed phase-12 PFX, emitting a
    PDF with a valid /ByteRange + PKCS#7 /Contents at ``output_path``.

    The unsigned source must already carry a signature field named
    "Signature" (placed there by ``generate_unsigned_sigfield_pdf`` via
    fpdf2's signature-field stub: in practice fpdf2 emits the empty
    AcroForm /Sig field structure, and pyhanko fills in /ByteRange +
    /Contents during ``sign_pdf``.
    """
    from pyhanko.pdf_utils.incremental_writer import IncrementalPdfFileWriter
    from pyhanko.sign import signers, fields
    from pyhanko.sign.signers.pdf_signer import (
        PdfSigner,
        PdfSignatureMetadata,
    )

    signer = signers.SimpleSigner.load_pkcs12(
        pfx_file=str(pfx_path),
        passphrase=pfx_password.encode("utf-8"),
    )

    with open(unsigned_path, "rb") as f:
        writer = IncrementalPdfFileWriter(f, strict=False)

        # Ensure there is a signature field named "Signature".  If the
        # unsigned source already has one (fpdf2 placeholder), pyhanko
        # will reuse it; otherwise we append.
        from pyhanko.sign.fields import enumerate_sig_fields
        existing = [name for (name, _val, _ref) in enumerate_sig_fields(writer)]
        if "Signature" not in existing:
            fields.append_signature_field(
                writer,
                sig_field_spec=fields.SigFieldSpec(
                    sig_field_name="Signature",
                    on_page=0,
                    # Coords picked to overlay the bottom "Signed: ___"
                    # underscore band drawn by generate_unsigned_sigfield_pdf
                    # (fpdf2 uses 210x297mm A4 landscape-orientation pts).
                    box=(72, 600, 360, 640),
                ),
            )

        pdf_signer = PdfSigner(
            PdfSignatureMetadata(
                field_name="Signature",
                reason="Phase-12 E2E test fixture — DO NOT USE IN PRODUCTION",
                location="PDFE test suite",
            ),
            signer=signer,
        )
        with open(output_path, "wb") as outf:
            pdf_signer.sign_pdf(writer, output=outf)

    print(f"Generated REAL signed PDF: {output_path} "
          f"({output_path.stat().st_size} bytes)")


def main():
    fixtures_dir = Path(__file__).parent

    # 1. Legacy PFX (kept for old tests).
    pfx_path = fixtures_dir / "test_signer.pfx"
    generate_pfx(pfx_path, password="test123")

    # 2. Unsigned PDF with sig-field placeholder.
    unsigned_path = fixtures_dir / "test_unsigned_sigfield.pdf"
    generate_unsigned_sigfield_pdf(unsigned_path)

    # 3. REAL signed PDF — phase-12 fixture upgrade.  Sign the same
    # source as the unsigned fixture (separate emission so the
    # unsigned-source still has an empty /Sig field for the live
    # sign-via-dialog tests).
    phase12_pfx = fixtures_dir / "certs" / "phase12_test_signer.pfx"
    phase12_password = "testpw123"

    if not phase12_pfx.exists():
        print(f"[warn] {phase12_pfx} missing — cannot regenerate "
              f"test_signed.pdf with a real signature.")
        print("       Run: see tests/fixtures/certs/README.md for the "
              "regeneration recipe.")
        return

    # Build a fresh unsigned source so we don't disturb the live
    # test_unsigned_sigfield.pdf fixture (its signature field must
    # remain empty for sign-via-dialog tests).
    unsigned_for_signing = fixtures_dir / "_unsigned_for_signing.pdf"
    generate_unsigned_sigfield_pdf(unsigned_for_signing)

    signed_path = fixtures_dir / "test_signed.pdf"
    try:
        generate_real_signed_pdf(
            unsigned_for_signing, signed_path,
            phase12_pfx, phase12_password,
        )
    finally:
        # Clean up the scratch unsigned file used solely as signing input.
        if unsigned_for_signing.exists():
            try:
                unsigned_for_signing.unlink()
            except OSError:
                pass

    print()
    print("Done.  test_signed.pdf now carries a real /ByteRange + PKCS#7 "
          "/Contents; run generate_tampered_pdf.py next to regenerate "
          "test_tampered.pdf from this baseline.")


if __name__ == "__main__":
    main()
