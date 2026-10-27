# -*- coding: utf-8 -*-
"""
Generate test certificate fixtures for PDF signature testing.

Outputs (all written to tests/fixtures/certs/):
  - test_signer_expired.pfx          — Self-signed, expired 2020-01-01
  - test_signer_selfsigned.pfx       — Self-signed leaf cert, valid 2024-2030
  - test_signer_intermediate.pfx     — Leaf signed by intermediate CA (full chain)
  - test_intermediate_ca.pem         — Intermediate CA cert in PEM format
  - test_root_ca.pem                 — Root CA cert in PEM format

All PFX files use password "test123".

Requires: pip install cryptography
"""

import sys
import os
from pathlib import Path
from datetime import datetime, timezone

from cryptography import x509
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives.serialization import pkcs12

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

SCRIPT_DIR = Path(__file__).resolve().parent
CERTS_DIR = SCRIPT_DIR / "certs"
CERTS_DIR.mkdir(parents=True, exist_ok=True)

PASSWORD = b"test123"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_rsa_key() -> rsa.RSAPrivateKey:
    """Generate a 2048-bit RSA private key."""
    return rsa.generate_private_key(public_exponent=65537, key_size=2048)


def _dt(year: int, month: int, day: int) -> datetime:
    """Return a UTC-aware datetime."""
    return datetime(year, month, day, tzinfo=timezone.utc)


def _subject(cn: str, o: str = "PDFE Test", ou: str = "QA", c: str = "US") -> x509.Name:
    return x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, cn),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, o),
        x509.NameAttribute(NameOID.ORGANIZATIONAL_UNIT_NAME, ou),
        x509.NameAttribute(NameOID.COUNTRY_NAME, c),
    ])


def _write_pfx(
    path: Path,
    key: rsa.RSAPrivateKey,
    cert: x509.Certificate,
    chain: list[x509.Certificate] | None = None,
    password: bytes = PASSWORD,
) -> None:
    """Serialize key + cert (+ optional chain) to a PKCS#12 / PFX file."""
    pfx_bytes = pkcs12.serialize_key_and_certificates(
        name=cert.subject.get_attributes_for_oid(NameOID.COMMON_NAME)[0].value.encode("utf-8"),
        key=key,
        cert=cert,
        cas=chain,
        encryption_algorithm=serialization.BestAvailableEncryption(password),
    )
    path.write_bytes(pfx_bytes)


def _write_pem(path: Path, cert: x509.Certificate) -> None:
    """Write a certificate in PEM format (UTF-8 text)."""
    pem_bytes = cert.public_bytes(serialization.Encoding.PEM)
    path.write_bytes(pem_bytes)


def _report(path: Path) -> None:
    size = path.stat().st_size
    print(f"  {path}  ({size:,} bytes)")


# ---------------------------------------------------------------------------
# 1. Expired self-signed cert  (2019-01-01 → 2020-01-01)
# ---------------------------------------------------------------------------

def generate_expired() -> None:
    print("Generating test_signer_expired.pfx ...")

    key = _make_rsa_key()
    subject = _subject("PDFE Test Expired Signer")

    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(subject)          # self-signed → issuer == subject
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(_dt(2019, 1, 1))
        .not_valid_after(_dt(2020, 1, 1))
        # Minimal extensions for a leaf cert
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(key.public_key()),
            critical=False,
        )
        .add_extension(
            x509.BasicConstraints(ca=False, path_length=None),
            critical=True,
        )
        .sign(key, hashes.SHA256())
    )

    out = CERTS_DIR / "test_signer_expired.pfx"
    _write_pfx(out, key, cert)
    _report(out)


# ---------------------------------------------------------------------------
# 2. Self-signed leaf cert  (2024-01-01 → 2030-01-01)
# ---------------------------------------------------------------------------

def generate_selfsigned() -> None:
    print("Generating test_signer_selfsigned.pfx ...")

    key = _make_rsa_key()
    subject = _subject("PDFE Test Self-Signed")

    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(_dt(2024, 1, 1))
        .not_valid_after(_dt(2030, 1, 1))
        # Pure leaf: CA=False, no cRLDistributionPoints, no OCSP
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(key.public_key()),
            critical=False,
        )
        .add_extension(
            x509.BasicConstraints(ca=False, path_length=None),
            critical=True,
        )
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                content_commitment=True,   # non-repudiation
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

    out = CERTS_DIR / "test_signer_selfsigned.pfx"
    _write_pfx(out, key, cert)
    _report(out)


# ---------------------------------------------------------------------------
# 3. Intermediate-CA chain  (2024-01-01 → 2030-01-01)
# ---------------------------------------------------------------------------

def generate_intermediate() -> None:
    print("Generating intermediate CA chain ...")

    # --- Root CA ---
    root_key = _make_rsa_key()
    root_subject = _subject("PDFE Test Root CA")

    root_cert = (
        x509.CertificateBuilder()
        .subject_name(root_subject)
        .issuer_name(root_subject)
        .public_key(root_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(_dt(2024, 1, 1))
        .not_valid_after(_dt(2030, 1, 1))
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(root_key.public_key()),
            critical=False,
        )
        .add_extension(
            x509.BasicConstraints(ca=True, path_length=1),
            critical=True,
        )
        .add_extension(
            x509.KeyUsage(
                digital_signature=False,
                content_commitment=False,
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=True,
                crl_sign=True,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .sign(root_key, hashes.SHA256())
    )

    # --- Intermediate CA (signed by root) ---
    int_key = _make_rsa_key()
    int_subject = _subject("PDFE Test Intermediate CA")

    int_cert = (
        x509.CertificateBuilder()
        .subject_name(int_subject)
        .issuer_name(root_subject)
        .public_key(int_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(_dt(2024, 1, 1))
        .not_valid_after(_dt(2030, 1, 1))
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(int_key.public_key()),
            critical=False,
        )
        .add_extension(
            x509.AuthorityKeyIdentifier.from_issuer_public_key(root_key.public_key()),
            critical=False,
        )
        .add_extension(
            x509.BasicConstraints(ca=True, path_length=0),
            critical=True,
        )
        .add_extension(
            x509.KeyUsage(
                digital_signature=False,
                content_commitment=False,
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=True,
                crl_sign=True,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .sign(root_key, hashes.SHA256())
    )

    # --- Leaf cert (signed by intermediate) ---
    leaf_key = _make_rsa_key()
    leaf_subject = _subject("PDFE Test Intermediate Signer")

    leaf_cert = (
        x509.CertificateBuilder()
        .subject_name(leaf_subject)
        .issuer_name(int_subject)
        .public_key(leaf_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(_dt(2024, 1, 1))
        .not_valid_after(_dt(2030, 1, 1))
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(leaf_key.public_key()),
            critical=False,
        )
        .add_extension(
            x509.AuthorityKeyIdentifier.from_issuer_public_key(int_key.public_key()),
            critical=False,
        )
        .add_extension(
            x509.BasicConstraints(ca=False, path_length=None),
            critical=True,
        )
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                content_commitment=True,
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
        .add_extension(
            x509.ExtendedKeyUsage([ExtendedKeyUsageOID.EMAIL_PROTECTION]),
            critical=False,
        )
        .sign(int_key, hashes.SHA256())
    )

    # PFX: leaf key + leaf cert + chain [intermediate, root]
    out_pfx = CERTS_DIR / "test_signer_intermediate.pfx"
    _write_pfx(out_pfx, leaf_key, leaf_cert, chain=[int_cert, root_cert])
    _report(out_pfx)

    # PEM files for trust store injection
    out_int_pem = CERTS_DIR / "test_intermediate_ca.pem"
    _write_pem(out_int_pem, int_cert)
    _report(out_int_pem)

    out_root_pem = CERTS_DIR / "test_root_ca.pem"
    _write_pem(out_root_pem, root_cert)
    _report(out_root_pem)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> int:
    print(f"Writing certificate fixtures to: {CERTS_DIR}\n")

    generate_expired()
    generate_selfsigned()
    generate_intermediate()

    print("\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
