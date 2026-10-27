#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate tests/fixtures/flaw_corpus/encryption/identity_stmf.pdf (PDF-25).

The fixture is a *standard security handler* document (/Filter /Standard,
/V 4 /R 4, 128-bit) whose stream crypt filter is ``/StmF /Identity``.

That combination is legal PDF and is the point of the gap: the file carries a
real ``/Encrypt`` dictionary, so every "is this encrypted?" check says YES,
while every STREAM in the body -- page content, images, embedded files -- is
stored in plaintext and can be read straight out of the file with no password
at all.  The string filter is left as the real ``/StdCF`` (AESV2) so the
document is not merely "unencrypted with a decorative dictionary"; the file
body simply contains no strings, so nothing needs AES to be written.

Passwords: user = "" (empty), owner = "owner".

Crypto used here is MD5 + RC4 only (Algorithm 2, 3 and 5 of ISO 32000-1
7.6.3.3/7.6.3.4), both from the standard library plus a 12-line RC4, so this
script has no third-party dependency.

Run from the repo root:

    python tests/fixtures/gen_identity_stmf.py
"""

from __future__ import annotations

import hashlib
import pathlib
import struct
import sys

# ISO 32000-1, 7.6.3.3 -- the 32-byte padding string.
PAD = bytes([
    0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41,
    0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
    0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80,
    0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A,
])

REVISION = 4
VERSION = 4
KEY_BYTES = 16          # /Length 128
PERMISSIONS = -4        # /P
OWNER_PASSWORD = b"owner"
USER_PASSWORD = b""     # empty user password
# Fixed so the fixture is byte-reproducible.
DOC_ID = bytes.fromhex("504446452d504446322d3235004964656e74")[:16]


def rc4(key: bytes, data: bytes) -> bytes:
    """Plain RC4.  Used only to BUILD a deliberately weak test fixture."""
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xFF
        s[i], s[j] = s[j], s[i]
    out = bytearray()
    i = j = 0
    for byte in data:
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out.append(byte ^ s[(s[i] + s[j]) & 0xFF])
    return bytes(out)


def pad_password(pw: bytes) -> bytes:
    return (pw + PAD)[:32]


def compute_o() -> bytes:
    """Algorithm 3 -- computing the /O (owner password) value."""
    digest = hashlib.md5(pad_password(OWNER_PASSWORD)).digest()
    for _ in range(50):                       # R >= 3
        digest = hashlib.md5(digest[:KEY_BYTES]).digest()
    key = digest[:KEY_BYTES]
    value = rc4(key, pad_password(USER_PASSWORD))
    for i in range(1, 20):                    # R >= 3
        value = rc4(bytes(b ^ i for b in key), value)
    return value


def compute_key(o_value: bytes) -> bytes:
    """Algorithm 2 -- computing the file encryption key."""
    md5 = hashlib.md5()
    md5.update(pad_password(USER_PASSWORD))
    md5.update(o_value)
    md5.update(struct.pack("<i", PERMISSIONS))
    md5.update(DOC_ID)
    # /EncryptMetadata is true, so the R4 0xFFFFFFFF step is NOT applied.
    digest = md5.digest()
    for _ in range(50):                       # R >= 3
        digest = hashlib.md5(digest[:KEY_BYTES]).digest()
    return digest[:KEY_BYTES]


def compute_u(key: bytes) -> bytes:
    """Algorithm 5 -- computing the /U (user password) value for R >= 3."""
    digest = hashlib.md5(PAD + DOC_ID).digest()
    value = rc4(key, digest)
    for i in range(1, 20):
        value = rc4(bytes(b ^ i for b in key), value)
    return value + b"\x00" * 16              # padded to 32 bytes


def build_pdf() -> bytes:
    o_value = compute_o()
    key = compute_key(o_value)
    u_value = compute_u(key)

    # Body content.  NOTE: no PDF string object appears anywhere in the body,
    # so the (real, AESV2) /StrF never has to encrypt anything -- which is what
    # lets this fixture be built without an AES implementation while still
    # declaring an honest string filter.
    content = (
        b"BT /F1 18 Tf 72 720 Td "
        b"(IDENTITY-STMF-PLAINTEXT-SENTINEL) Tj ET\n"
    )
    # ...except the page's own content stream, which IS a stream and therefore
    # takes /StmF -- /Identity -- so it stays plaintext.  That is the defect
    # under test, and the sentinel above is greppable in the raw file bytes.

    objects: list[bytes] = [
        b"<</Type/Catalog/Pages 2 0 R>>",
        b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
        b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]"
        b"/Resources<</Font<</F1 5 0 R>>>>/Contents 4 0 R>>",
        b"<</Length " + str(len(content)).encode() + b">>\nstream\n"
        + content + b"endstream",
        b"<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>",
        b"<</Filter/Standard/V " + str(VERSION).encode()
        + b"/R " + str(REVISION).encode()
        + b"/Length " + str(KEY_BYTES * 8).encode()
        + b"/CF<</StdCF<</CFM/AESV2/AuthEvent/DocOpen/Length "
        + str(KEY_BYTES).encode() + b">>>>"
        + b"/StmF/Identity"          # <-- the condition under test
        + b"/StrF/StdCF"
        + b"/P " + str(PERMISSIONS).encode()
        + b"/EncryptMetadata true"
        + b"/O<" + o_value.hex().encode() + b">"
        + b"/U<" + u_value.hex().encode() + b">>>",
    ]

    out = bytearray(b"%PDF-1.6\n%\xe2\xe3\xcf\xd3\n")
    offsets: list[int] = []
    for num, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += str(num).encode() + b" 0 obj\n" + body + b"\nendobj\n"

    xref_at = len(out)
    out += b"xref\n0 " + str(len(objects) + 1).encode() + b"\n"
    out += b"0000000000 65535 f \n"
    for off in offsets:
        out += ("%010d 00000 n \n" % off).encode()
    idhex = DOC_ID.hex().encode()
    out += (
        b"trailer\n<</Size " + str(len(objects) + 1).encode()
        + b"/Root 1 0 R/Encrypt 6 0 R/ID[<" + idhex + b"><" + idhex + b">]>>\n"
        b"startxref\n" + str(xref_at).encode() + b"\n%%EOF\n"
    )
    return bytes(out)


def main() -> int:
    target = (
        pathlib.Path(__file__).resolve().parent
        / "flaw_corpus" / "encryption" / "identity_stmf.pdf"
    )
    target.parent.mkdir(parents=True, exist_ok=True)
    data = build_pdf()
    with open(target, "wb") as handle:
        handle.write(data)
    print(f"wrote {target} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
