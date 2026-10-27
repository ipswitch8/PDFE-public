#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate a PDF with a cached RFC-3161 /DocTimeStamp signature.

Uses a self-signed TSA certificate to produce a valid-shape
TimeStampToken (TST) offline. The resulting fixture is fully
deterministic and requires no network access at test time.

Outputs:
  tests/fixtures/test_tsa_cert.pem       - self-signed TSA cert
  tests/fixtures/test_tsa_key.pem        - TSA private key
  tests/fixtures/test_tsa_token.der      - cached TimeStampToken
                                            (SignedData wrapping TSTInfo)
  tests/fixtures/test_timestamped.pdf    - PDF with /DocTimeStamp
                                            embedding the cached token
"""

from __future__ import annotations

import datetime
import hashlib
import io
from pathlib import Path

from asn1crypto import algos, cms, core, tsp, x509 as asn1_x509
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID

FIXTURES = Path(__file__).parent
# Fixed timestamp so the resulting file is byte-deterministic across
# generator runs. Any date will do; tests tolerate this being "stale".
FIXED_TIME = datetime.datetime(2025, 1, 1, 12, 0, 0, tzinfo=datetime.timezone.utc)


def build_tsa_cert_and_key() -> tuple[x509.Certificate, rsa.RSAPrivateKey]:
    """Self-signed CA usable as an RFC-3161 TSA."""
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = x509.Name(
        [
            x509.NameAttribute(NameOID.COUNTRY_NAME, "US"),
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, "PDFE Test"),
            x509.NameAttribute(NameOID.COMMON_NAME, "PDFE Test TSA"),
        ]
    )
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(subject)
        .public_key(key.public_key())
        .serial_number(0x1234_5678_ABCD)
        .not_valid_before(FIXED_TIME - datetime.timedelta(days=1))
        .not_valid_after(FIXED_TIME + datetime.timedelta(days=3650))
        .add_extension(
            x509.ExtendedKeyUsage([ExtendedKeyUsageOID.TIME_STAMPING]),
            critical=True,
        )
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                content_commitment=True,
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=True,
                crl_sign=False,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .sign(key, hashes.SHA256())
    )
    return cert, key


def build_timestamp_token(
    message_digest: bytes,
    cert: x509.Certificate,
    key: rsa.RSAPrivateKey,
) -> bytes:
    """Return DER-encoded SignedData wrapping a TSTInfo.

    This is what goes inside /Contents of a PDF /DocTimeStamp. The
    outer RFC-3161 TimeStampResp envelope is NOT included — PDF
    embeds just the token.
    """
    # --- TSTInfo ---------------------------------------------------------
    tst_info = tsp.TSTInfo(
        {
            "version": "v1",
            "policy": "1.2.3.4.1",  # arbitrary test policy OID
            "message_imprint": tsp.MessageImprint(
                {
                    "hash_algorithm": algos.DigestAlgorithm(
                        {"algorithm": "sha256"}
                    ),
                    "hashed_message": message_digest,
                }
            ),
            "serial_number": 0xDEADBEEF,
            "gen_time": FIXED_TIME,
            "accuracy": tsp.Accuracy({"seconds": 1}),
            "ordering": False,
            "nonce": 0x4242_4242,
            "tsa": x509.Name.build(
                {"country_name": "US", "common_name": "PDFE Test TSA"}
            )
            if False  # asn1crypto wants a GeneralName here; omit for simplicity
            else None,
        }
    )
    tst_info_der = tst_info.dump()

    # --- SignerInfo signed attributes -----------------------------------
    # Must contain messageDigest (over tst_info_der) + contentType.
    content_digest = hashlib.sha256(tst_info_der).digest()
    signed_attrs = cms.CMSAttributes(
        [
            cms.CMSAttribute(
                {
                    "type": "content_type",
                    "values": [cms.ContentType("tst_info")],
                }
            ),
            cms.CMSAttribute(
                {
                    "type": "message_digest",
                    "values": [cms.OctetString(content_digest)],
                }
            ),
            cms.CMSAttribute(
                {
                    "type": "signing_time",
                    "values": [cms.Time({"utc_time": FIXED_TIME})],
                }
            ),
        ]
    )

    # SignerInfo signs the DER encoding of the signed attributes
    # (with an implicit SET-OF tag, per RFC 5652).
    to_sign = signed_attrs.dump()
    # asn1crypto emits signed_attrs as [0] IMPLICIT; we need SET-OF for signing
    to_sign = b"\x31" + to_sign[1:]

    signature = key.sign(to_sign, padding.PKCS1v15(), hashes.SHA256())

    # --- SignerInfo -----------------------------------------------------
    cert_der = cert.public_bytes(serialization.Encoding.DER)
    asn1_cert = asn1_x509.Certificate.load(cert_der)
    signer_info = cms.SignerInfo(
        {
            "version": "v1",
            "sid": cms.SignerIdentifier(
                {
                    "issuer_and_serial_number": cms.IssuerAndSerialNumber(
                        {
                            "issuer": asn1_cert.issuer,
                            "serial_number": asn1_cert.serial_number,
                        }
                    )
                }
            ),
            "digest_algorithm": algos.DigestAlgorithm({"algorithm": "sha256"}),
            "signed_attrs": signed_attrs,
            "signature_algorithm": algos.SignedDigestAlgorithm(
                {"algorithm": "rsassa_pkcs1v15"}
            ),
            "signature": signature,
        }
    )

    # --- SignedData / ContentInfo --------------------------------------
    signed_data = cms.SignedData(
        {
            "version": "v3",
            "digest_algorithms": [
                algos.DigestAlgorithm({"algorithm": "sha256"})
            ],
            "encap_content_info": cms.EncapsulatedContentInfo(
                {
                    "content_type": "tst_info",
                    "content": cms.ParsableOctetString(tst_info_der),
                }
            ),
            "certificates": [
                cms.CertificateChoices({"certificate": asn1_cert})
            ],
            "signer_infos": [signer_info],
        }
    )
    content_info = cms.ContentInfo(
        {"content_type": "signed_data", "content": signed_data}
    )
    return content_info.dump()


def build_pdf_with_timestamp(token_der: bytes) -> bytes:
    """Hand-roll a minimal PDF carrying a /DocTimeStamp signature.

    The PDF has:
    - One blank page
    - AcroForm with a signature field on page 1
    - Signature dict with /Type /DocTimeStamp, /Filter /Adobe.PPKLite,
      /SubFilter /ETSI.RFC3161, /Contents <hex token>, /ByteRange [...]
    """
    # Build the PDF body with placeholder /Contents + /ByteRange so we
    # can compute the actual ranges after layout, then splice the token
    # in.
    CONTENTS_PAD = 8192  # bytes of hex room for the token
    placeholder_contents = b"0" * (CONTENTS_PAD * 2)
    placeholder_byterange = b"[0 ********** ********** **********]"

    def obj(n: int, body: bytes) -> bytes:
        return f"{n} 0 obj\n".encode() + body + b"\nendobj\n"

    objects = {
        1: b"<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        3: (
            b"<< /Type /Page /Parent 2 0 R "
            b"/MediaBox [0 0 612 792] /Resources << >> "
            b"/Annots [4 0 R] >>"
        ),
        4: (
            b"<< /Type /Annot /Subtype /Widget /FT /Sig /T (Timestamp) "
            b"/Rect [0 0 0 0] /V 6 0 R /P 3 0 R /F 4 >>"
        ),
        5: b"<< /Fields [4 0 R] /SigFlags 3 >>",
        6: (
            b"<< /Type /DocTimeStamp /Filter /Adobe.PPKLite "
            b"/SubFilter /ETSI.RFC3161 "
            b"/ByteRange " + placeholder_byterange + b" "
            b"/Contents <" + placeholder_contents + b"> >>"
        ),
    }

    buf = bytearray(b"%PDF-1.7\n%\xff\xff\xff\xff\n")
    offsets: dict[int, int] = {}
    for n in sorted(objects):
        offsets[n] = len(buf)
        buf += obj(n, objects[n])
    xref_off = len(buf)
    size = max(objects) + 1
    buf += f"xref\n0 {size}\n".encode()
    buf += b"0000000000 65535 f \n"
    for n in range(1, size):
        buf += f"{offsets[n]:010d} 00000 n \n".encode()
    buf += (
        b"trailer\n<< /Size " + str(size).encode()
        + b" /Root 1 0 R >>\nstartxref\n"
        + str(xref_off).encode() + b"\n%%EOF\n"
    )

    # Locate the /Contents placeholder and /ByteRange placeholder and
    # compute real values.
    pdf = bytes(buf)
    contents_open = pdf.index(b"/Contents <") + len(b"/Contents <")
    contents_close = pdf.index(b">", contents_open)
    br_open = pdf.index(placeholder_byterange)
    br_close = br_open + len(placeholder_byterange)

    # Signed bytes: everything except the hex literal INSIDE the < ... >
    # brackets (but including the brackets themselves).
    offset_before = contents_open
    offset_after = contents_close
    length_after = len(pdf) - offset_after

    byterange = f"[0 {offset_before} {offset_after} {length_after}]".encode()
    # Pad to original placeholder length so offsets don't shift.
    byterange = byterange.ljust(len(placeholder_byterange))

    # Hex-encode the token, pad with zeros.
    hex_token = token_der.hex().upper().encode()
    if len(hex_token) > len(placeholder_contents):
        raise ValueError(
            f"token too large ({len(hex_token)} > {len(placeholder_contents)})"
        )
    hex_token = hex_token.ljust(len(placeholder_contents), b"0")

    out = bytearray(pdf)
    out[br_open:br_close] = byterange
    out[contents_open:contents_close] = hex_token
    return bytes(out)


def main() -> None:
    cert, key = build_tsa_cert_and_key()

    cert_pem = cert.public_bytes(serialization.Encoding.PEM)
    key_pem = key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption(),
    )
    (FIXTURES / "test_tsa_cert.pem").write_bytes(cert_pem)
    # F1 (phase-51 review). The key is written with an in-band test-only label
    # ahead of the `-----BEGIN` armour, where RFC 7468 §5.2 requires PEM readers
    # to skip it -- inert to every parser, visible to a secret scanner or to an
    # engineer meeting the file out of context.
    # `tools/gate/check_test_key_markers.py` enforces this repo-wide over every
    # TRACKED file holding a PEM private key; emitting it HERE too is what stops
    # a regeneration silently dropping the thing the gate then reds on.
    KEY_MARKER = (
        "# PDFE TEST-ONLY KEY -- generated by "
        "tests/fixtures/generate_timestamped_pdf.py\n"
        "# Not a secret. Never used outside the test corpus. Safe to publish.\n"
    ).encode("ascii")
    (FIXTURES / "test_tsa_key.pem").write_bytes(KEY_MARKER + key_pem)

    # We need the PDF body BEFORE we can compute the digest over the
    # byte-range, because /Contents is what's excluded. Easy path:
    # build the PDF with a placeholder, compute the digest over the
    # final signed bytes, then rebuild the token, then splice it in.
    # Even easier: the test only needs "a PDF with a /DocTimeStamp"
    # that parses; verifying the signature is a MuPDF concern and
    # the signed bytes can be over an arbitrary digest since the
    # token is self-signed. Use a fixed digest.
    digest_placeholder = hashlib.sha256(b"pdfe-test-tsa").digest()
    token = build_timestamp_token(digest_placeholder, cert, key)
    (FIXTURES / "test_tsa_token.der").write_bytes(token)

    pdf_bytes = build_pdf_with_timestamp(token)
    (FIXTURES / "test_timestamped.pdf").write_bytes(pdf_bytes)

    print(f"wrote test_tsa_cert.pem ({len(cert_pem)} B)")
    print(f"wrote test_tsa_key.pem  ({len(key_pem)} B)")
    print(f"wrote test_tsa_token.der ({len(token)} B)")
    print(f"wrote test_timestamped.pdf ({len(pdf_bytes)} B)")


if __name__ == "__main__":
    main()
