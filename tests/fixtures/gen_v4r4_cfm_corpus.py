#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate the /V 4 /R 4 crypt-filter-method (CFM) discrimination pair.

Meta-audit 2026-07 follow-up, phase **F24** (residual item raised by the F3
``security-audit`` gate).

``EncryptionManager::getMethodString`` used to answer ``AES-128`` for every
``/V 4 /R 4`` document, deriving the cipher from the handler revision alone.
``/V 4`` is precisely the version that decoupled those two things: the revision
says how the file key is derived, and the ``/CF`` entry named by ``/StmF`` says
which cipher that key drives.  ``/CFM /AESV2`` is AES-128; ``/CFM /V2`` is
RC4-128, is legal at ``/V 4 /R 4`` (ISO 32000-1 Table 25), and was being
reported as AES — **overstating the protection**, the same mis-label class F3
closed for ``/V 1``, in the opposite direction.

Two fixtures, identical in every respect except the crypt-filter method, so a
reader that does not consult ``/CFM`` cannot tell them apart:

``v4r4_cfm_v2.pdf``
    ``/CF << /StdCF << /CFM /V2 ... >> >>`` — RC4-128.  MUST be reported as
    ``RC4-128``.
``v4r4_cfm_aesv2.pdf``
    ``/CF << /StdCF << /CFM /AESV2 ... >> >>`` — genuine AES-128.  The CONTROL:
    it must still be reported as ``AES-128``, otherwise the fix would just be a
    blanket re-label.

Both use the empty user password (so MuPDF opens them without a prompt) and
owner password ``owner``; both encrypt streams AND strings with ``/StdCF``, so
the declared filter is the filter actually in force.

Key derivation is Algorithm 2/3/5 of ISO 32000-1 7.6.3.3-7.6.3.4 and the
per-object key is Algorithm 1 (7.6.2), the same MD5+RC4 code shape as
``gen_identity_stmf.py``.  AES-128-CBC for the AESV2 fixture comes from
``cryptography`` (already a fixture-generation dependency — see
``gen_sig_shadow_corpus.py``).

Regenerate with::

    python tests/fixtures/gen_v4r4_cfm_corpus.py
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

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
USER_PASSWORD = b""     # empty user password -> opens with no prompt

# Fixed so both fixtures are byte-reproducible.
DOC_ID = bytes.fromhex("504446452d4632342d5634523443464d")[:16]
# Fixed AES IV, for the same reason. A test fixture is not a secret.
AES_IV = bytes.fromhex("0f1e2d3c4b5a69788796a5b4c3d2e1f0")

SENTINEL = b"V4R4-CFM-DISCRIMINATION-SENTINEL"


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


def object_key(file_key: bytes, num: int, gen: int, aes: bool) -> bytes:
    """Algorithm 1 -- the per-object key."""
    md5 = hashlib.md5()
    md5.update(file_key)
    md5.update(bytes([num & 0xFF, (num >> 8) & 0xFF, (num >> 16) & 0xFF]))
    md5.update(bytes([gen & 0xFF, (gen >> 8) & 0xFF]))
    if aes:
        md5.update(b"\x73\x41\x6C\x54")      # "sAlT"
    return md5.digest()[:min(len(file_key) + 5, 16)]


def encrypt_object_data(file_key: bytes, num: int, gen: int, data: bytes,
                        cfm: str) -> bytes:
    key = object_key(file_key, num, gen, aes=(cfm == "AESV2"))
    if cfm == "V2":
        return rc4(key, data)
    # AESV2: AES-128-CBC, IV prepended, PKCS#7 padding (ISO 32000-1 7.6.2).
    padding = 16 - (len(data) % 16)
    padded = data + bytes([padding]) * padding
    encryptor = Cipher(algorithms.AES(key), modes.CBC(AES_IV)).encryptor()
    return AES_IV + encryptor.update(padded) + encryptor.finalize()


def build_pdf(cfm: str) -> bytes:
    """Author a /V 4 /R 4 document whose /StdCF uses crypt-filter method `cfm`."""
    o_value = compute_o()
    file_key = compute_key(o_value)
    u_value = compute_u(file_key)

    plain_content = (
        b"BT /F1 18 Tf 72 720 Td (" + SENTINEL + b") Tj ET\n"
    )
    # The page content stream is object 4; /StmF is /StdCF, so it is encrypted
    # with the per-object key under the declared crypt-filter method. That is
    # what makes the declared /CFM the REAL one rather than a decorative name.
    content = encrypt_object_data(file_key, 4, 0, plain_content, cfm)

    objects: list[bytes] = [
        b"<</Type/Catalog/Pages 2 0 R>>",
        b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
        b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]"
        b"/Resources<</Font<</F1 5 0 R>>>>/Contents 4 0 R>>",
        b"<</Length " + str(len(content)).encode() + b">>\nstream\n"
        + content + b"\nendstream",
        b"<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>",
        b"<</Filter/Standard/V " + str(VERSION).encode()
        + b"/R " + str(REVISION).encode()
        + b"/Length " + str(KEY_BYTES * 8).encode()
        + b"/CF<</StdCF<</CFM/" + cfm.encode()
        + b"/AuthEvent/DocOpen/Length " + str(KEY_BYTES).encode() + b">>>>"
        + b"/StmF/StdCF"
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
    target_dir = (
        pathlib.Path(__file__).resolve().parent / "flaw_corpus" / "encryption"
    )
    target_dir.mkdir(parents=True, exist_ok=True)

    manifest: dict[str, object] = {
        "generator": "tests/fixtures/gen_v4r4_cfm_corpus.py",
        "phase": "F24 (meta-audit-2026-07 follow-up phase-5)",
        "synthetic": True,
        "user_password": "",
        "owner_password": OWNER_PASSWORD.decode(),
        "fixtures": {},
    }
    fx: dict[str, object] = manifest["fixtures"]  # type: ignore[assignment]

    for cfm, name, expect_method, expect_stream_method in (
        ("V2", "v4r4_cfm_v2.pdf", "RC4-128", "RC4"),
        ("AESV2", "v4r4_cfm_aesv2.pdf", "AES-128", "AES"),
    ):
        data = build_pdf(cfm)
        (target_dir / name).write_bytes(data)
        fx[name] = {
            "cfm": cfm,
            "version": VERSION,
            "revision": REVISION,
            "key_length_bits": KEY_BYTES * 8,
            "expect_method": expect_method,
            "expect_stream_crypt_method": expect_stream_method,
            "bytes": len(data),
            "role": ("the defect: /V 4 /R 4 that is NOT AES"
                     if cfm == "V2"
                     else "control: /V 4 /R 4 that genuinely IS AES-128"),
        }
        print("wrote %-24s %6d bytes  /CFM /%s -> expect %s"
              % (name, len(data), cfm, expect_method))

    (target_dir / "v4r4_cfm_manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print("wrote v4r4_cfm_manifest.json (%d fixtures)" % len(fx))
    return 0


if __name__ == "__main__":
    sys.exit(main())
