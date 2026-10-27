# -*- coding: utf-8 -*-
"""
Generator for the adversarial-ambient corpus (pipeline e5b9c231 Phase 3, AC1).

One minimal, hand-built content-stream fixture per hostile ambient-state class
that a text-edit injector must survive. Each page leaves a hostile graphics/text
state live at the point an edit is injected, and carries an editable "EDITME"
span plus a "KEEPME" neighbour, so a test can assert BOTH the edited element and
the untouched neighbour render correctly (visual quad/argb oracle).

Classes covered here (input-domain-audit.md ranked backlog):
  * adv_cmyk_fill.pdf     - row 1: ambient CMYK (`k`) text fill
  * adv_dangling_q.pdf    - row 3: dangling/unbalanced `q` (leaked gstate)
  * adv_clip_path.pdf     - row 4: active clip path around the edit region

(Rows 2 and 6 -- ambient Tc/Tw/Ts and unbalanced `cm` -- are already covered by
the Phase-2 fixtures state_leak_ts.pdf / ambient_half_scale_page.pdf /
ambient_neg_tz_page.pdf; see docs/audit/phase3-adversarial-corpus.md.)

Regenerate with:  python tests/fixtures/gen_adversarial_ambient_corpus.py
"""
import io, os

H = 200


def _pdf(content):
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 %d] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>" % H,
        b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    out = io.BytesIO()
    out.write(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for i, obj in enumerate(objs, 1):
        offsets.append(out.tell())
        out.write(b"%d 0 obj\n" % i + obj + b"\nendobj\n")
    xref = out.tell()
    n = len(objs) + 1
    out.write(b"xref\n0 %d\n" % n + b"0000000000 65535 f \n")
    for k in range(1, n):
        out.write(b"%010d 00000 n \n" % offsets[k])
    out.write(b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (n, xref))
    return out.getvalue()


# Upright standard text (positive font, no Y-flip), so the visual oracle reads a
# clean left-to-right run; the hostile ambient state is layered on top.

# Row 1: ambient CMYK (`k`) fill. Both spans are drawn cyan; an editor that does
# not capture/re-emit a visible colour would render the replacement black.
CMYK = (
    b"q\nBT\n/F1 18 Tf\n"
    b"1 0 0 0 k\n"                  # ambient CMYK cyan fill (never reset)
    b"60 120 Td\n(EDITME) Tj\n"
    b"0 -40 Td (KEEPME) Tj\n"
    b"ET\nQ\n"
)

# Row 3: a dangling `q` pushes a grey fill that is never popped; content after it
# (and any injected edit) inherits the leaked grey unless neutralised.
DANGLING_Q = (
    b"q\n0.6 0.6 0.6 rg\n"          # pushed grey, never restored (dangling q)
    b"BT\n/F1 18 Tf\n"
    b"0 0 0 rg\n"                    # local black for the spans
    b"60 120 Td\n(EDITME) Tj\n"
    b"0 -40 Td (KEEPME) Tj\n"
    b"ET\n"                          # note: the q above is left dangling (no Q)
)

# Row 4: an active clip path around the edit region. The edit must land inside
# the clip (visible), not be clipped away.
CLIP = (
    b"q\n20 20 260 160 re W n\n"    # clip to a box covering the text area
    b"BT\n/F1 18 Tf\n0 0 0 rg\n"
    b"60 120 Td\n(EDITME) Tj\n"
    b"0 -40 Td (KEEPME) Tj\n"
    b"ET\nQ\n"
)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    for name, content in [
        ("adv_cmyk_fill.pdf", CMYK),
        ("adv_dangling_q.pdf", DANGLING_Q),
        ("adv_clip_path.pdf", CLIP),
    ]:
        with open(os.path.join(here, name), "wb") as f:
            f.write(_pdf(content))
        print("wrote", name)


if __name__ == "__main__":
    main()
