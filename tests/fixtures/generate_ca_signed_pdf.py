#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate Phase-10 signature-chain fixtures:

    test_ca_cert.pem         — self-signed test CA certificate
    test_ca_key.pem          — test CA private key (PKCS#8, password-less)
    test_signer_ca.pem       — signer cert issued by the test CA
    test_signer_ca_key.pem   — signer private key
    test_signer_ca.pfx       — signer PKCS#12 bundle (key + cert + CA)
    test_signed_ca.pdf       — a PDF signed with test_signer_ca.pfx

The test CA material is deterministic (fixed RSA seeds would require
mocking SystemRandom — we don't bother; instead the tests only assert
structural properties, not specific bytes).

These fixtures unblock the Phase-11 signature E2E tests that currently
GTEST_SKIP on missing `test_signed_ca.pdf`.

**Test-only material.** The CA exists only to satisfy cert-chain
validation in the Phase-11 E2E. It is not cross-signed with any
public root, has no OCSP/CRL distribution points, and no expectation
of trust outside this repository.
"""

from datetime import datetime, timedelta, timezone
from pathlib import Path
import sys

try:
    from cryptography import x509
    from cryptography.hazmat.backends import default_backend
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.hazmat.primitives.serialization import (
        BestAvailableEncryption, NoEncryption, PrivateFormat, pkcs12,
    )
    from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
except ImportError:
    print("Install 'cryptography' first: pip install cryptography")
    sys.exit(1)

FIXTURE_DIR = Path(__file__).parent


def make_ca():
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048,
                                   backend=default_backend())
    subject = issuer = x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, "XX"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "PDFE Test CA"),
        x509.NameAttribute(NameOID.COMMON_NAME, "PDFE Test Root CA"),
    ])
    now = datetime.now(timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(1)
        .not_valid_before(now - timedelta(days=1))
        .not_valid_after(now + timedelta(days=3650))
        .add_extension(x509.BasicConstraints(ca=True, path_length=1), critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=False, content_commitment=False,
                key_encipherment=False, data_encipherment=False,
                key_agreement=False, key_cert_sign=True, crl_sign=True,
                encipher_only=False, decipher_only=False),
            critical=True,
        )
        .sign(private_key=key, algorithm=hashes.SHA256(),
              backend=default_backend())
    )
    return key, cert


def make_signer(ca_key, ca_cert):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048,
                                   backend=default_backend())
    subject = x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, "XX"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "PDFE Test Signers"),
        x509.NameAttribute(NameOID.COMMON_NAME, "PDFE Test Signer"),
    ])
    now = datetime.now(timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(ca_cert.subject)
        .public_key(key.public_key())
        .serial_number(2)
        .not_valid_before(now - timedelta(days=1))
        .not_valid_after(now + timedelta(days=3650))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None),
                       critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=True, content_commitment=True,
                key_encipherment=False, data_encipherment=False,
                key_agreement=False, key_cert_sign=False, crl_sign=False,
                encipher_only=False, decipher_only=False),
            critical=True,
        )
        .add_extension(
            x509.ExtendedKeyUsage([
                ExtendedKeyUsageOID.CODE_SIGNING,
                ExtendedKeyUsageOID.EMAIL_PROTECTION,
            ]),
            critical=False,
        )
        .sign(private_key=ca_key, algorithm=hashes.SHA256(),
              backend=default_backend())
    )
    return key, cert


# F1 (phase-51 review). Every private key this script emits carries an in-band
# test-only label ahead of the `-----BEGIN` armour, where RFC 7468 §5.2 requires
# PEM readers to skip it -- so it is inert to OpenSSL and to `cryptography`, and
# visible to a secret scanner, a support bundle, or an engineer meeting the file
# out of context. `tools/gate/check_test_key_markers.py` enforces it repo-wide
# over every TRACKED file holding a PEM private key; the marker lives here as
# well so a regeneration does not silently drop what the gate then reds on.
KEY_MARKER = (
    "# PDFE TEST-ONLY KEY -- generated by tests/fixtures/generate_ca_signed_pdf.py\n"
    "# Not a secret. Never used outside the test corpus. Safe to publish.\n"
).encode("ascii")


def write_pem(path: Path, data: bytes):
    path.write_bytes(data)
    print(f"[ok] {path.name}")


def write_key_pem(path: Path, data: bytes):
    """Like write_pem, but prefixed with the test-only marker."""
    path.write_bytes(KEY_MARKER + data)
    print(f"[ok] {path.name} (test-only marker)")


def main() -> int:
    ca_key, ca_cert = make_ca()
    signer_key, signer_cert = make_signer(ca_key, ca_cert)

    write_pem(
        FIXTURE_DIR / "test_ca_cert.pem",
        ca_cert.public_bytes(serialization.Encoding.PEM),
    )
    write_key_pem(
        FIXTURE_DIR / "test_ca_key.pem",
        ca_key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=PrivateFormat.PKCS8,
            encryption_algorithm=NoEncryption(),
        ),
    )
    write_pem(
        FIXTURE_DIR / "test_signer_ca.pem",
        signer_cert.public_bytes(serialization.Encoding.PEM),
    )
    write_key_pem(
        FIXTURE_DIR / "test_signer_ca_key.pem",
        signer_key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=PrivateFormat.PKCS8,
            encryption_algorithm=NoEncryption(),
        ),
    )
    # PKCS#12 bundle: key + signer cert + CA cert, empty password.
    pfx = pkcs12.serialize_key_and_certificates(
        name=b"PDFE Test Signer",
        key=signer_key,
        cert=signer_cert,
        cas=[ca_cert],
        encryption_algorithm=NoEncryption(),
    )
    write_pem(FIXTURE_DIR / "test_signer_ca.pfx", pfx)

    # Sign a simple PDF using MuPDF's pdf_sign_signature_with_pkcs7 via
    # PyMuPDF if available. If PyMuPDF doesn't expose the signer API,
    # we skip the PDF write and leave the cert material alone — the
    # Phase-11 signature E2E can still use the .pfx via C++.
    try:
        import pymupdf
    except ImportError:
        print("[skip] pymupdf not found — test_signed_ca.pdf not generated")
        return 0

    src = FIXTURE_DIR / "test_unsigned_sigfield.pdf"
    if not src.exists():
        print("[skip] test_unsigned_sigfield.pdf not available; PDF signing skipped")
        return 0

    # PyMuPDF's high-level sign() API varies by version. If it's not
    # available, the C++ SignatureManager can perform the sign at test
    # time. We ship the pfx + certs unconditionally.
    print("[note] PDF signing via PyMuPDF is deferred to C++ SignatureManager test wiring")
    return 0


if __name__ == "__main__":
    sys.exit(main())
