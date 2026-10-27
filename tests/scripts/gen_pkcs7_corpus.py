# -*- coding: utf-8 -*-
"""Generate PKCS#7 fuzzer corpus seeds for SEC-PKCS7-FUZZ."""

import os
import sys
from pathlib import Path

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.hazmat.primitives.serialization import pkcs7, pkcs12
    import datetime
except ImportError:
    print("cryptography package not installed; generating minimal seeds only",
          file=sys.stderr)
    # Generate just the trivial seeds without cryptography
    corpus_dir = Path(__file__).resolve().parent.parent / "corpus" / "pkcs7"
    corpus_dir.mkdir(parents=True, exist_ok=True)

    (corpus_dir / "zero_byte.bin").write_bytes(b"")
    (corpus_dir / "truncated.bin").write_bytes(
        b"\x30\x82\x00\x10\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x07\x02")
    (corpus_dir / "garbage.bin").write_bytes(os.urandom(256))
    print(f"Wrote 3 minimal seeds to {corpus_dir}")
    sys.exit(0)

corpus_dir = Path(__file__).resolve().parent.parent / "corpus" / "pkcs7"
corpus_dir.mkdir(parents=True, exist_ok=True)

# Seed 0: zero-byte input
(corpus_dir / "zero_byte.bin").write_bytes(b"")

# Seed 1: truncated PKCS7 header (OID 1.2.840.113549.1.7.2 = signedData)
(corpus_dir / "truncated.bin").write_bytes(
    b"\x30\x82\x00\x10\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x07\x02")

# Seed 2: random garbage
(corpus_dir / "garbage.bin").write_bytes(os.urandom(256))

# Seed 3: valid self-signed PKCS7 SignedData
key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
subject = x509.Name([
    x509.NameAttribute(x509.oid.NameOID.COMMON_NAME, "PDFE Fuzzer Seed"),
])
now = datetime.datetime.now(datetime.timezone.utc)
cert = (
    x509.CertificateBuilder()
    .subject_name(subject)
    .issuer_name(subject)
    .public_key(key.public_key())
    .serial_number(x509.random_serial_number())
    .not_valid_before(now)
    .not_valid_after(now + datetime.timedelta(days=365))
    .sign(key, hashes.SHA256())
)

signed_data = (
    pkcs7.PKCS7SignatureBuilder()
    .set_data(b"test content for fuzzer seed")
    .add_signer(cert, key, hashes.SHA256())
    .sign(serialization.Encoding.DER, [pkcs7.PKCS7Options.DetachedSignature])
)
(corpus_dir / "valid_signed.bin").write_bytes(signed_data)

# Seed 4: valid PKCS7 with flipped bytes (simulates corruption)
corrupted = bytearray(signed_data)
if len(corrupted) > 50:
    corrupted[40] ^= 0xFF
    corrupted[41] ^= 0xFF
(corpus_dir / "corrupted_signed.bin").write_bytes(bytes(corrupted))

print(f"Wrote 5 seeds to {corpus_dir}")
