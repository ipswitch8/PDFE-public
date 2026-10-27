# -*- coding: utf-8 -*-
"""Generate the two "document already HAS a /DSS" fixtures used by
tests/test_dss_save_fault_injection.cpp (meta-audit follow-up F57, phase-52).

Why these have to be committed rather than built at test time
------------------------------------------------------------
LtvDssWriter's rollback has two structurally different restore routes:

  * /Root/DSS is a DIRECT dict   -> restored with pdf_dict_puts()
  * /Root/DSS is an INDIRECT ref -> restored with pdf_update_object()

Every /DSS PDFE itself writes is the direct-dict shape (writeDssDict uses
pdf_dict_puts_dict), so a fixture produced by PDFE can only ever probe the
first route.  The indirect shape is what Acrobat and every other producer that
keeps the catalog small emits, and before F57 it was unprobed in BOTH
directions -- neither the successful restore nor the failed-capture path had a
document that could reach it.  A hand-built file is the only way to get one.

Output (both derived from test_single.pdf, both structurally identical apart
from the one difference under test):

  dss_pre_direct.pdf    /Root/DSS is a direct dictionary
  dss_pre_indirect.pdf  /Root/DSS is "8 0 R", an indirect reference

Both carry a one-element /Certs array pointing at a real (indirect) stream, so
"the /DSS was silently mutated" is observable as a change in the /Certs count
rather than only as its presence.  The stream body is not a real certificate --
nothing in the rollback path parses it, and a fixture that needed a valid X.509
would have to be regenerated whenever it expired.

The test asserts pdf_is_indirect() on /Root/DSS at run time (via
DssCounts::dssIsIndirect); it does not take this script's word for it.

Regenerate with:
    python tests/fixtures/make_dss_pre_fixtures.py
"""

import re
import sys
from pathlib import Path

FIXTURES = Path(__file__).resolve().parent
SOURCE = FIXTURES / "test_single.pdf"

# A short, deliberately non-certificate payload for the pre-existing /Certs
# stream. See the module docstring for why it is not a real DER blob.
CERT_STREAM_BODY = b"PDFE-F57-PREEXISTING-DSS-CERT-PLACEHOLDER"


def parse_objects(data: bytes) -> dict:
    """Map object number -> raw body bytes (everything between "N 0 obj" and
    "endobj"). test_single.pdf is an uncompressed, single-revision file with no
    object streams, which is what makes this crude scan sufficient."""
    objects = {}
    for m in re.finditer(rb"(\d+)\s+0\s+obj\b", data):
        num = int(m.group(1))
        end = data.find(b"endobj", m.end())
        if end < 0:
            raise RuntimeError(f"object {num} has no endobj")
        objects[num] = data[m.end():end]
    if not objects:
        raise RuntimeError("no objects found in source PDF")
    return objects


def build(dss_indirect: bool) -> bytes:
    data = SOURCE.read_bytes()
    objects = parse_objects(data)

    catalog = objects[2]
    if b"/Type /Catalog" not in catalog and b"/Type/Catalog" not in catalog:
        raise RuntimeError("object 2 is not the catalog; source PDF changed")
    if b"/DSS" in catalog:
        raise RuntimeError("source catalog already has a /DSS")

    dss_body = b"<<\n/Certs [9 0 R]\n>>"

    if dss_indirect:
        catalog = catalog.replace(b"/Type /Catalog", b"/DSS 8 0 R\n/Type /Catalog")
        objects[8] = b"\n" + dss_body + b"\n"
    else:
        catalog = catalog.replace(b"/Type /Catalog",
                                  b"/DSS " + dss_body + b"\n/Type /Catalog")
    objects[2] = catalog

    objects[9] = (b"\n<<\n/Length %d\n>>\nstream\n" % len(CERT_STREAM_BODY)
                  + CERT_STREAM_BODY + b"\nendstream\n")

    out = bytearray()
    out += b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"

    numbers = sorted(objects)
    offsets = {}
    for num in numbers:
        offsets[num] = len(out)
        out += b"%d 0 obj" % num
        out += objects[num]
        out += b"endobj\n"

    size = max(numbers) + 1
    xref_offset = len(out)
    out += b"xref\n0 %d\n" % size
    out += b"0000000000 65535 f \n"
    for num in range(1, size):
        if num in offsets:
            out += b"%010d 00000 n \n" % offsets[num]
        else:
            # No object with this number exists (the source PDF is dense, so
            # this branch is defensive only). A free entry keeps the table the
            # contiguous 0..Size-1 run the spec requires.
            out += b"0000000000 65535 f \n"

    out += (b"trailer\n<<\n/Size %d\n/Root 2 0 R\n/Info 7 0 R\n"
            b"/ID [<F57F57F57F57F57F57F57F57F57F57F5>"
            b"<F57F57F57F57F57F57F57F57F57F57F5>]\n>>\n" % size)
    out += b"startxref\n%d\n%%%%EOF\n" % xref_offset
    return bytes(out)


def main() -> int:
    if not SOURCE.exists():
        print(f"missing source fixture: {SOURCE}", file=sys.stderr)
        return 1
    for indirect, name in ((False, "dss_pre_direct.pdf"),
                           (True, "dss_pre_indirect.pdf")):
        blob = build(indirect)
        (FIXTURES / name).write_bytes(blob)
        print(f"wrote {name} ({len(blob)} bytes, "
              f"/Root/DSS {'indirect' if indirect else 'direct'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
