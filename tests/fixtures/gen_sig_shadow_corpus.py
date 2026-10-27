#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generator for the F21 adversarial signature corpus (gaps PDF-33/36/37).

Meta-audit 2026-07 follow-up, phase F21.

Every file this script emits is a SYNTHETIC document hand-authored here and
signed with the repository's own committed test key material
(``tests/fixtures/test_signer.pfx``, password ``test123``).  **Nothing in this
corpus is a captured or adapted exploit against third-party software.**  The
attacks modelled are structural properties of the PDF signature format itself
(ISO 32000-1 §12.8), and every fixture targets PDFE's own parsing.

What the corpus models
----------------------

``sig_clean_signed.pdf``
    The over-refusal CONTROL.  Really signed, untampered, ``/ByteRange`` covers
    the whole file except the ``/Contents`` hole.  Every "we detected it"
    assertion elsewhere is meaningless unless this one comes back clean.

``sig_orphan_widget_signed.pdf``
    The F26 UNDER-WARN regression fixture.  Really signed, untampered, whole
    file covered — but the document catalog has no ``/AcroForm``, so the
    signature field is unreachable from ``/Root /AcroForm /Fields`` and is
    visible only as a widget in the page's ``/Annots``.  A one-property change
    from ``sig_clean_signed.pdf``.  Any component that answers "where are the
    signatures?" by walking the AcroForm field tree alone reports ZERO here,
    which is how ``removeEncryption`` came to destroy such a signature in
    silence.

``sig_shadow_hide.pdf`` / ``sig_shadow_replace.pdf``
    The incremental-update SHADOW ATTACK.  The signed byte range is
    byte-identical to ``sig_clean_signed.pdf`` — the PKCS#7 digest still
    verifies — but an appended incremental update redefines the page content
    stream, so the document a human reads is not the document that was signed.
    ``hide`` conceals the original wording behind an opaque box; ``replace``
    substitutes different wording outright.

``sig_wrapping_two_byteranges.pdf``
    SIGNATURE WRAPPING.  A block of injected objects is spliced into the middle
    of the file and the ``/ByteRange`` is widened so the second range starts
    *after* it — leaving a hole in the signed coverage that is NOT the
    ``/Contents`` hex string.  The signature is then RE-SIGNED over the
    manipulated ranges, so the digest is genuinely valid: the only thing wrong
    with this file is that the ``/ByteRange`` does not cover it.  That is
    exactly the property PDF-37 says nothing tests.  The injected block carries
    a second ``/ByteRange`` array, hence the name.

``sig_docmdp_multirev.pdf``
    An attacker-added REVISION CHAIN against a /P 1 certified document.  Three
    incremental updates are appended, each of which rewrites the document
    catalog — the object DocMdpEnforcer uses to advance its certification
    anchor.  The anchor advance is deliberately capped at one revision; this
    fixture is what turns that desk-check into a test.

``sig_fieldmdp_locked.pdf`` / ``sig_fieldmdp_unlocked.pdf``
    The FieldMDP (PDF-33) discriminating PAIR.  Both are certified at /P 2
    (form filling permitted) and both carry a ``/Lock`` naming ``locked_field``.
    The first has an incremental update that changes ``locked_field``; the
    second changes ``free_field``.  Everything else about them is identical, so
    a detector that cannot read ``/Lock`` cannot tell them apart.

``sig_iso_docmdp_p1.pdf`` / ``sig_iso_docmdp_p3.pdf``
    ISO-layout certification.  ``/Perms /DocMDP`` is the SIGNATURE dictionary
    and the level lives in ``/Reference [ << /TransformMethod /DocMDP
    /TransformParams << /P n >> >> ]`` — the layout Acrobat and pyHanko emit.
    PDFE's own writer instead puts ``/TransformParams`` directly on the
    ``/Perms /DocMDP`` dict, and until F21 the reader understood only that.

Ground truth
------------

Every byte count a test asserts is AUTHORED HERE and written to
``sig_shadow_corpus_manifest.json``.  The tests read the manifest; they never
anchor to whatever PDFE happens to report.

Regenerate with::

    python tests/fixtures/gen_sig_shadow_corpus.py
"""

from __future__ import annotations

import datetime
import hashlib
import json
from pathlib import Path

from asn1crypto import cms, algos, x509 as asn1_x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives.serialization import pkcs12

FIXTURES = Path(__file__).resolve().parent
PFX_PATH = FIXTURES / "test_signer.pfx"
PFX_PASSWORD = b"test123"

# Fixed so the corpus is byte-deterministic across regenerations.
FIXED_TIME = datetime.datetime(2026, 1, 15, 9, 30, 0, tzinfo=datetime.timezone.utc)
PDF_DATE = b"D:20260115093000Z"

# Size of the /Contents hex string, INCLUDING the enclosing angle brackets.
# Generous enough for a 2048-bit RSA CMS SignedData with one certificate.
CONTENTS_HOLE = 8192

ORIGINAL_LINE = b"Pay ACME Corporation the sum of 100 USD."
REPLACED_LINE = b"Pay EVIL Holdings the sum of 100000 USD."
WRAPPING_MARKER = b"PDFE-F21-WRAPPED-PAYLOAD-NOT-COVERED-BY-BYTERANGE"


# ---------------------------------------------------------------------------
# Detached CMS SignedData over a PDF byte-range digest.
# ---------------------------------------------------------------------------

def load_signer():
    key, cert, _extra = pkcs12.load_key_and_certificates(
        PFX_PATH.read_bytes(), PFX_PASSWORD
    )
    return key, cert


def detached_cms(signed_bytes: bytes, key, cert) -> bytes:
    """Return DER ``ContentInfo``/``SignedData`` detached over ``signed_bytes``.

    Shape matches what ``adbe.pkcs7.detached`` requires: no eContent, signed
    attributes carrying contentType/messageDigest/signingTime.
    """
    message_digest = hashlib.sha256(signed_bytes).digest()
    signed_attrs = cms.CMSAttributes([
        cms.CMSAttribute({
            "type": "content_type",
            "values": [cms.ContentType("data")],
        }),
        cms.CMSAttribute({
            "type": "signing_time",
            "values": [cms.Time({"utc_time": FIXED_TIME})],
        }),
        cms.CMSAttribute({
            "type": "message_digest",
            "values": [cms.OctetString(message_digest)],
        }),
    ])
    # RFC 5652: the signature is over the DER SET OF encoding of the signed
    # attributes, not over the [0] IMPLICIT form asn1crypto emits in place.
    to_sign = b"\x31" + signed_attrs.dump()[1:]
    signature = key.sign(to_sign, padding.PKCS1v15(), hashes.SHA256())

    asn1_cert = asn1_x509.Certificate.load(
        cert.public_bytes(serialization.Encoding.DER)
    )
    signer_info = cms.SignerInfo({
        "version": "v1",
        "sid": cms.SignerIdentifier({
            "issuer_and_serial_number": cms.IssuerAndSerialNumber({
                "issuer": asn1_cert.issuer,
                "serial_number": asn1_cert.serial_number,
            })
        }),
        "digest_algorithm": algos.DigestAlgorithm({"algorithm": "sha256"}),
        "signed_attrs": signed_attrs,
        "signature_algorithm": algos.SignedDigestAlgorithm(
            {"algorithm": "rsassa_pkcs1v15"}
        ),
        "signature": signature,
    })
    signed_data = cms.SignedData({
        "version": "v1",
        "digest_algorithms": [algos.DigestAlgorithm({"algorithm": "sha256"})],
        # Detached: content_type only, no eContent — that is what
        # adbe.pkcs7.detached means. asn1crypto selects the ContentInfo spec
        # for a v1 SignedData.
        "encap_content_info": cms.ContentInfo({"content_type": "data"}),
        "certificates": [cms.CertificateChoices({"certificate": asn1_cert})],
        "signer_infos": [signer_info],
    })
    return cms.ContentInfo(
        {"content_type": "signed_data", "content": signed_data}
    ).dump()


# ---------------------------------------------------------------------------
# Raw PDF authoring helpers.
# ---------------------------------------------------------------------------

def obj(num: int, body: bytes) -> bytes:
    return b"%d 0 obj\n" % num + body + b"\nendobj\n"


def stream_obj(num: int, data: bytes, extra: bytes = b"") -> bytes:
    head = b"<< /Length %d%s >>" % (len(data), extra)
    return (b"%d 0 obj\n" % num + head + b"\nstream\n" + data
            + b"\nendstream\nendobj\n")


def content_stream(lines: list[tuple[int, int, bytes]]) -> bytes:
    out = bytearray(b"BT\n")
    for x, y, text in lines:
        out += b"/F1 14 Tf 1 0 0 1 %d %d Tm (%s) Tj\n" % (x, y, text)
    out += b"ET\n"
    return bytes(out)


def xref_section(offsets: dict[int, int], size: int) -> tuple[bytes, list[int]]:
    """Full (non-incremental) xref table for objects 1..size-1."""
    out = bytearray(b"xref\n0 %d\n" % size)
    out += b"0000000000 65535 f \n"
    for n in range(1, size):
        out += b"%010d 00000 n \n" % offsets[n]
    return bytes(out), []


def incremental_update(base: bytes, prev_xref: int, size: int,
                       objects: dict[int, bytes],
                       root_num: int = 1) -> bytes:
    """Append an incremental update revising ``objects`` (num -> full body).

    ``objects`` values are complete ``N 0 obj ... endobj`` blocks minus the
    wrapper, i.e. just the object body bytes.
    """
    out = bytearray(base)
    offsets: dict[int, int] = {}
    for num in sorted(objects):
        offsets[num] = len(out)
        out += obj(num, objects[num])
    xref_at = len(out)
    out += b"xref\n"
    # One subsection per contiguous run of object numbers.
    nums = sorted(offsets)
    i = 0
    while i < len(nums):
        j = i
        while j + 1 < len(nums) and nums[j + 1] == nums[j] + 1:
            j += 1
        out += b"%d %d\n" % (nums[i], j - i + 1)
        for k in range(i, j + 1):
            out += b"%010d 00000 n \n" % offsets[nums[k]]
        i = j + 1
    out += (b"trailer\n<< /Size %d /Root %d 0 R /Prev %d >>\n"
            % (size, root_num, prev_xref))
    out += b"startxref\n%010d\n%%%%EOF\n" % xref_at
    return bytes(out)


# ---------------------------------------------------------------------------
# The signed base document.
# ---------------------------------------------------------------------------

def build_unsigned_base(*, perms: bytes = b"", sig_extra: bytes = b"",
                        lock_obj: bytes | None = None,
                        text_fields: bool = False,
                        link_acroform: bool = True) -> tuple[bytes, int, int]:
    """Author the base document with a placeholder signature.

    Returns ``(bytes, base_xref_offset, next_free_object_number)``.

    Object layout is deliberate: the signature value dictionary (object 8) is
    the LAST object before the xref table, so the wrapping construction can
    splice bytes in immediately after its ``/Contents`` hole and only have to
    repair ``startxref``.

    ``link_acroform=False`` drops ``/AcroForm`` from the document catalog. The
    AcroForm dictionary (object 6) is still WRITTEN, it is simply no longer
    reachable from ``/Root`` — the broken-linkage shape a malformed or
    hostile file takes. The signature widget (object 7) remains in the page's
    ``/Annots``, so the signature is fully visible to any reader that
    enumerates widgets (MuPDF's ``pdf_first_widget``, which is how PDFE's own
    ``SignatureManager`` lists signatures) and invisible to one that walks only
    ``/Root /AcroForm /Fields``. See ``build_orphan_widget``.
    """
    field_refs = b"7 0 R"
    annots = b"7 0 R"
    if text_fields:
        field_refs += b" 10 0 R 11 0 R"
        annots += b" 10 0 R 11 0 R"

    lock_entry = b" /Lock 9 0 R" if lock_obj is not None else b""

    bodies: dict[int, bytes] = {}
    acroform_entry = b" /AcroForm 6 0 R" if link_acroform else b""
    bodies[1] = (b"<< /Type /Catalog /Pages 2 0 R" + acroform_entry + perms
                 + b" >>")
    bodies[2] = b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>"
    bodies[3] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
                 b" /Resources << /Font << /F1 5 0 R >> >>"
                 b" /Contents 4 0 R /Annots [" + annots + b"] >>")
    bodies[5] = (b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica"
                 b" /Encoding /WinAnsiEncoding >>")
    bodies[6] = (b"<< /Fields [" + field_refs + b"] /SigFlags 3"
                 b" /DA (/F1 0 Tf 0 g)"
                 b" /DR << /Font << /F1 5 0 R >> >> >>")
    bodies[7] = (b"<< /Type /Annot /Subtype /Widget /FT /Sig /T (Signature1)"
                 b" /Rect [400 60 560 110] /P 3 0 R /F 4 /V 8 0 R"
                 + lock_entry + b" >>")
    if lock_obj is not None:
        bodies[9] = lock_obj
    if text_fields:
        bodies[10] = (b"<< /Type /Annot /Subtype /Widget /FT /Tx"
                      b" /T (locked_field) /V (original-locked)"
                      b" /Rect [72 600 300 630] /P 3 0 R /F 4"
                      b" /DA (/F1 12 Tf 0 g) >>")
        bodies[11] = (b"<< /Type /Annot /Subtype /Widget /FT /Tx"
                      b" /T (free_field) /V (original-free)"
                      b" /Rect [72 550 300 580] /P 3 0 R /F 4"
                      b" /DA (/F1 12 Tf 0 g) >>")

    lines = [(72, 700, b"PDFE F21 adversarial signature corpus"),
             (72, 660, ORIGINAL_LINE)]
    if text_fields:
        lines.append((72, 500, b"locked_field = original-locked"))
        lines.append((72, 470, b"free_field = original-free"))

    size = 12 if text_fields else (10 if lock_obj is not None else 9)

    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets: dict[int, int] = {}

    for num in sorted(bodies):
        offsets[num] = len(out)
        out += obj(num, bodies[num])
    # Content stream (object 4).
    offsets[4] = len(out)
    out += stream_obj(4, content_stream(lines))

    # Object 8 — the signature value dictionary — LAST before the xref.
    offsets[8] = len(out)
    sig_body = (b"<< /Type /Sig /Filter /Adobe.PPKLite"
                b" /SubFilter /adbe.pkcs7.detached"
                b" /Name (PDFE Test Signer) /M (" + PDF_DATE + b")"
                b" /Reason (F21 adversarial corpus) /Location (Test Lab)"
                + sig_extra +
                b" /ByteRange [0 0000000000 0000000000 0000000000]"
                b" /Contents <" + b"0" * (CONTENTS_HOLE - 2) + b"> >>")
    out += obj(8, sig_body)

    base_xref = len(out)
    for n in range(1, size):
        offsets.setdefault(n, 0)
    xref, _ = xref_section(offsets, size)
    out += xref
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\n" % size
    out += b"startxref\n%010d\n%%%%EOF\n" % base_xref
    return bytes(out), base_xref, size


def sign_in_place(raw: bytes, key, cert,
                  byte_range: tuple[int, int, int, int] | None = None) -> bytes:
    """Patch ``/ByteRange`` and ``/Contents`` of the single placeholder sig.

    With ``byte_range`` omitted the honest full-coverage range is written.
    Passing one explicitly is how the wrapping fixture states a range that
    leaves a gap — and it is still genuinely signed OVER that stated range,
    which is the whole point: the digest is valid and the coverage is not.
    """
    data = bytearray(raw)
    br_at = data.find(b"/ByteRange [0 ")
    assert br_at >= 0, "no /ByteRange placeholder"
    br_end = data.find(b"]", br_at) + 1
    hole_start = data.find(b"/Contents <", br_end)
    assert hole_start >= 0, "no /Contents placeholder"
    a = hole_start + len(b"/Contents ")           # offset of '<'
    close = data.find(b">", a)
    b = close + 1                                  # offset just past '>'

    if byte_range is None:
        byte_range = (0, a, b, len(data) - b)
    r0, r1, r2, r3 = byte_range
    new_br = b"/ByteRange [%d %010d %010d %010d]" % (r0, r1, r2, r3)
    assert len(new_br) == br_end - br_at, (
        "byte-range field width changed: %d vs %d"
        % (len(new_br), br_end - br_at))
    data[br_at:br_end] = new_br

    signed = bytes(data[r0:r0 + r1]) + bytes(data[r2:r2 + r3])
    der = detached_cms(signed, key, cert)
    hex_sig = der.hex().upper().encode("ascii")
    room = (b - 1) - (a + 1)
    assert len(hex_sig) <= room, "CMS does not fit the /Contents hole"
    data[a + 1:b - 1] = hex_sig + b"0" * (room - len(hex_sig))
    return bytes(data)


def byte_range_of(raw: bytes) -> tuple[int, int, int, int]:
    at = raw.find(b"/ByteRange [")
    end = raw.find(b"]", at)
    parts = raw[at + len(b"/ByteRange ["):end].split()
    return tuple(int(p) for p in parts)  # type: ignore[return-value]


# ---------------------------------------------------------------------------
# Fixture builders.
# ---------------------------------------------------------------------------

def build_clean(key, cert):
    raw, base_xref, size = build_unsigned_base()
    return sign_in_place(raw, key, cert), base_xref, size


def build_orphan_widget(key, cert):
    """A REALLY-signed document whose signature is NOT in /Root /AcroForm.

    Meta-audit F26 REMEDIATION (security-audit gate finding).

    Identical to ``sig_clean_signed.pdf`` in every respect except one: the
    document catalog carries no ``/AcroForm`` entry, so the signature field is
    unreachable from ``/Root /AcroForm /Fields``. The signature widget is still
    in the page's ``/Annots`` and the signature is still genuine and still
    covers the whole file, so:

      * ``SignatureManager`` (``pdf_first_widget``) lists it,
      * ``DocMdpEnforcer`` (page ``/Annots``) attributes it,
      * and the pre-remediation ``removeEncryption`` probe, which walked only
        the AcroForm field tree, could not see it — so it destroyed the
        signature with no warning at all.

    That divergence is what this fixture pins. It is a ONE-PROPERTY change from
    the control, which is what makes it discriminating.
    """
    raw, base_xref, size = build_unsigned_base(link_acroform=False)
    return sign_in_place(raw, key, cert), base_xref, size


def build_shadow(key, cert, mode: str):
    """Append an incremental update redefining the page content stream."""
    signed, base_xref, size = build_clean(key, cert)
    if mode == "hide":
        # Draw an opaque box over the original wording, then restate it as
        # something innocuous. The signed bytes still say ORIGINAL_LINE.
        body = (content_stream([(72, 700,
                                 b"PDFE F21 adversarial signature corpus")])
                + b"1 1 1 rg 60 640 500 40 re f\n"
                + content_stream([(72, 660, b"(nothing to see here)")]))
    else:
        body = content_stream([
            (72, 700, b"PDFE F21 adversarial signature corpus"),
            (72, 660, REPLACED_LINE),
        ])
    stream = stream_obj(4, body)
    # incremental_update wants bodies, not whole objects; inline the stream.
    out = bytearray(signed)
    off4 = len(out)
    out += stream
    xref_at = len(out)
    out += b"xref\n4 1\n%010d 00000 n \n" % off4
    out += (b"trailer\n<< /Size %d /Root 1 0 R /Prev %d >>\n"
            % (size, base_xref))
    out += b"startxref\n%010d\n%%%%EOF\n" % xref_at
    appended = len(out) - len(signed)
    return bytes(out), appended


def build_wrapping(key, cert):
    """Splice injected objects into the middle of the signed file."""
    unsigned, base_xref, size = build_unsigned_base()
    provisional = sign_in_place(unsigned, key, cert)
    r0, r1, r2, r3 = byte_range_of(provisional)
    a, b = r1, r2                      # '<' offset, one past '>'

    # The injected block. It is inert as far as the document structure goes —
    # nothing references it — but it carries a marker and a DECOY /ByteRange
    # array so the file literally contains two of them.
    payload = (b"\n%% " + WRAPPING_MARKER + b"\n"
               b"90 0 obj\n<< /Type /Sig /Filter /Adobe.PPKLite"
               b" /SubFilter /adbe.pkcs7.detached"
               b" /ByteRange [0 0000000000 0000000000 0000000000]"
               b" /ShadowPayload (" + WRAPPING_MARKER + b") >>\nendobj\n"
               b"91 0 obj\n(" + REPLACED_LINE + b")\nendobj\n")
    # Pad to a round number so the manifest figure is memorable and any
    # off-by-one in the analyzer shows up as an obviously wrong count.
    assert len(payload) < 2048
    payload = payload + b"%" + b"P" * (2048 - len(payload) - 2) + b"\n"
    assert len(payload) == 2048

    # Splice AFTER object 8's `endobj`, not inside its dictionary: the
    # signature dictionary has to stay parseable, otherwise the analyzer
    # cannot read the very /ByteRange the fixture is about.
    splice = provisional.find(b"endobj\n", b) + len(b"endobj\n")
    assert splice > b
    spliced = bytearray(provisional[:splice]) + payload + provisional[splice:]
    # Repair startxref: everything after `splice` moved by len(payload).
    sx = spliced.rfind(b"startxref\n")
    old = int(spliced[sx + 10:sx + 20])
    spliced[sx + 10:sx + 20] = b"%010d" % (old + len(payload))

    gap = (splice - b) + len(payload)   # bytes between the hole and range 2
    # The second range is the ORIGINAL tail from the splice point onward — the
    # payload displaced it, so its length is not the original r3.
    tail = len(provisional) - splice
    final = sign_in_place(bytes(spliced), key, cert,
                          byte_range=(0, a, b + gap, tail))
    return final, gap, len(payload)


def build_iso_certified(key, cert, level: int):
    """/Perms /DocMDP = the signature dict; /P lives under /Reference."""
    ref = (b" /Reference [ << /Type /SigRef /TransformMethod /DocMDP"
           b" /TransformParams << /Type /TransformParams /V /1.2 /P %d >>"
           b" /DigestMethod /SHA256 >> ]" % level)
    raw, base_xref, size = build_unsigned_base(
        perms=b" /Perms << /DocMDP 8 0 R >>", sig_extra=ref)
    return sign_in_place(raw, key, cert), base_xref, size


def build_docmdp_multirev(key, cert):
    """P1-certified, then THREE attacker revisions that rewrite the catalog."""
    signed, base_xref, size = build_iso_certified(key, cert, 1)
    perms = b" /Perms << /DocMDP 8 0 R >>"
    catalog = (b"<< /Type /Catalog /Pages 2 0 R /AcroForm 6 0 R" + perms)

    def page(annots: bytes) -> bytes:
        return (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
                b" /Resources << /Font << /F1 5 0 R >> >>"
                b" /Contents 4 0 R /Annots [" + annots + b"] >>")

    def square(y: int) -> bytes:
        return (b"<< /Type /Annot /Subtype /Square /Rect [200 %d 260 %d]"
                b" /F 4 /P 3 0 R >>" % (y, y + 60))

    # Each revision rewrites the CATALOG — the object whose version the
    # enforcer's anchor advance keys off — and additionally revises one object
    # that NO LATER revision touches. Without that second half, the catalog
    # rewrites of the earlier revisions would be shadowed by the later ones and
    # the fixture could not show that every revision is still attributed.
    revisions = [
        {1: catalog + b" /PageLayout /SinglePage >>",
         9: square(200),
         3: page(b"7 0 R 9 0 R")},
        {1: catalog + b" /PageLayout /SinglePage /PageMode /UseNone >>",
         10: square(300),
         3: page(b"7 0 R 9 0 R 10 0 R")},
        {1: catalog + b" /PageLayout /TwoColumnLeft /PageMode /UseNone >>",
         11: square(400),
         3: page(b"7 0 R 9 0 R 10 0 R 11 0 R")},
    ]

    out = bytes(signed)
    prev = base_xref
    appended_per_rev = []
    for rev in revisions:
        before = len(out)
        size = max(size, max(rev) + 1)
        out = incremental_update(out, prev, size, rev)
        sx = out.rfind(b"startxref\n")
        prev = int(out[sx + 10:sx + 20])
        appended_per_rev.append(len(out) - before)
    return out, len(revisions), appended_per_rev


def build_fieldmdp(key, cert, which: str):
    """/P 2 certified + /Lock on locked_field; update one field or the other."""
    ref = (b" /Reference [ << /Type /SigRef /TransformMethod /DocMDP"
           b" /TransformParams << /Type /TransformParams /V /1.2 /P 2 >>"
           b" /DigestMethod /SHA256 >>"
           b" << /Type /SigRef /TransformMethod /FieldMDP"
           b" /TransformParams << /Type /TransformParams /V /1.2"
           b" /Action /Include /Fields [(locked_field)] >> >> ]")
    lock = (b"<< /Type /SigFieldLock /Action /Include"
            b" /Fields [(locked_field)] >>")
    raw, base_xref, size = build_unsigned_base(
        perms=b" /Perms << /DocMDP 8 0 R >>", sig_extra=ref,
        lock_obj=lock, text_fields=True)
    signed = sign_in_place(raw, key, cert)

    target, value = ((10, b"TAMPERED-locked") if which == "locked"
                     else (11, b"legitimate-free"))
    name = b"locked_field" if which == "locked" else b"free_field"
    body = (b"<< /Type /Annot /Subtype /Widget /FT /Tx"
            b" /T (" + name + b") /V (" + value + b")"
            b" /Rect [72 %d 300 %d] /P 3 0 R /F 4"
            b" /DA (/F1 12 Tf 0 g) >>"
            % ((600, 630) if which == "locked" else (550, 580)))
    out = incremental_update(signed, base_xref, size, {target: body})
    return out, len(out) - len(signed)


# ---------------------------------------------------------------------------

def main() -> None:
    key, cert = load_signer()
    manifest: dict[str, object] = {
        "generator": "tests/fixtures/gen_sig_shadow_corpus.py",
        "phase": "F21 (meta-audit-2026-07 follow-up phase-2)",
        "gap_ids": ["PDF-33", "PDF-36", "PDF-37"],
        "signer": "tests/fixtures/test_signer.pfx (password test123)",
        "synthetic": True,
        "fixtures": {},
    }
    fx: dict[str, object] = manifest["fixtures"]  # type: ignore[assignment]

    def emit(name: str, data: bytes, **facts):
        (FIXTURES / name).write_bytes(data)
        r0, r1, r2, r3 = byte_range_of(data)
        facts.update({
            "bytes": len(data),
            "byte_range": [r0, r1, r2, r3],
            "signed_bytes": r1 + r3,
            # The real /Contents hex string, always CONTENTS_HOLE bytes.
            "contents_hole_bytes": CONTENTS_HOLE,
            # What /ByteRange skips, which for the wrapping fixture is the hole
            # PLUS the injected block. The difference between these two figures
            # IS the attack.
            "byte_range_hole_bytes": r2 - (r0 + r1),
        })
        fx[name] = facts
        print("wrote %-36s %7d bytes  ByteRange %s"
              % (name, len(data), [r0, r1, r2, r3]))

    clean, _, _ = build_clean(key, cert)
    emit("sig_clean_signed.pdf", clean,
         role="control",
         expect_verdict="CoversWholeFile",
         unsigned_after_range=0, unsigned_in_gaps=0,
         byte_range_arrays_in_file=1,
         original_line=ORIGINAL_LINE.decode())

    orphan, _, _ = build_orphan_widget(key, cert)
    emit("sig_orphan_widget_signed.pdf", orphan,
         role="applied signature reachable ONLY from the page /Annots "
              "(no /Root /AcroForm) — F26 under-warn regression fixture",
         expect_verdict="CoversWholeFile",
         unsigned_after_range=0, unsigned_in_gaps=0,
         byte_range_arrays_in_file=1,
         acroform_linked_from_root=False,
         applied_signatures=1,
         original_line=ORIGINAL_LINE.decode())

    for mode in ("hide", "replace"):
        data, appended = build_shadow(key, cert, mode)
        emit("sig_shadow_%s.pdf" % mode, data,
             role="incremental-update shadow attack (%s)" % mode,
             expect_verdict="ContentAppendedAfterSignedRange",
             unsigned_after_range=appended, unsigned_in_gaps=0,
             byte_range_arrays_in_file=1,
             signed_prefix_matches="sig_clean_signed.pdf",
             appended_bytes=appended,
             original_line=ORIGINAL_LINE.decode(),
             rendered_line=(REPLACED_LINE.decode() if mode == "replace"
                            else "(nothing to see here)"))

    wrap, gap, injected = build_wrapping(key, cert)
    emit("sig_wrapping_two_byteranges.pdf", wrap,
         role="signature wrapping",
         expect_verdict="GapInSignedRange",
         unsigned_after_range=0, unsigned_in_gaps=gap,
         byte_range_arrays_in_file=2,
         gap_bytes=gap,
         injected_bytes=injected,
         gap_marker=WRAPPING_MARKER.decode())

    for level in (1, 3):
        data, _, _ = build_iso_certified(key, cert, level)
        emit("sig_iso_docmdp_p%d.pdf" % level, data,
             role="ISO-layout certification (/Reference /TransformParams)",
             expect_verdict="CoversWholeFile",
             unsigned_after_range=0, unsigned_in_gaps=0,
             byte_range_arrays_in_file=1,
             expect_level=level)

    multi, nrev, per_rev = build_docmdp_multirev(key, cert)
    emit("sig_docmdp_multirev.pdf", multi,
         role="attacker-added revision chain against a /P 1 certification",
         expect_verdict="ContentAppendedAfterSignedRange",
         unsigned_after_range=sum(per_rev), unsigned_in_gaps=0,
         byte_range_arrays_in_file=1,
         expect_level=1,
         attacker_revisions=nrev,
         catalog_rewrites=nrev,
         appended_per_revision=per_rev)

    for which in ("locked", "unlocked"):
        data, appended = build_fieldmdp(key, cert, which)
        emit("sig_fieldmdp_%s.pdf" % which, data,
             role="FieldMDP %s-field update (/P 2 certified)" % which,
             expect_verdict="ContentAppendedAfterSignedRange",
             unsigned_after_range=appended, unsigned_in_gaps=0,
             byte_range_arrays_in_file=1,
             expect_level=2,
             locked_fields=["locked_field"],
             changed_field=("locked_field" if which == "locked"
                            else "free_field"),
             expect_field_lock_violated=(which == "locked"))

    (FIXTURES / "sig_shadow_corpus_manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print("wrote sig_shadow_corpus_manifest.json (%d fixtures)" % len(fx))


if __name__ == "__main__":
    main()
