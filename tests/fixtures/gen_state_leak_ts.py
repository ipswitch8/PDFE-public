# -*- coding: utf-8 -*-
"""
Generator for tests/fixtures/state_leak_ts.pdf

Regression fixture for the INBOUND text-state leak in ContentStreamEditor
(Register H1 / docs/content-stream-state-continuity-audit.md, pipeline e5b9c231
Phase 2). It uses the SAME structural pattern that triggered the original -100
Tz mirror bug (neg_tz_grey_text.pdf):

  * the ambient text state is set INSIDE the BT and is never reset;
  * the edited span ("SHIP TO") is followed by a second span in the SAME text
    object, so the editor stays on the pdf_filter_page_contents pass-through
    path and emits its replacement injection INLINE (not hoisted), where the
    still-active ambient state is captured by the injection's saved gstate.

Here the ambient state is a text rise "50 Ts". The editor's injection emits
Tz/Tr/rg/Tf/Tm but never resets Ts (nor Tc/Tw), so the replacement inherits the
ambient +50 rise and is drawn 50 units above the position the editor computed
for it -- i.e. 50 units off the original span it replaced.

Empirical note: MuPDF's pdf_filter_page_contents wraps the injection in q...Q
and, for structurally simpler pages, hoists it above the page content -- which
neutralises the OUTBOUND leak and the inbound leak on those pages. This fixture
deliberately reproduces the one structural pattern where the ambient state is
still live inside the injection's scope, so the inbound leak is observable.

Regenerate with:  python tests/fixtures/gen_state_leak_ts.py
"""
import io, os

H = 200
CONTENT = (
    b"q q\n"
    b"1 0 0 -1 0 %d cm\n"        # Y-flip CTM (device-space origin at top-left)
    b"BT\n"
    b"-100 Tz\n"                  # ambient horizontal scale (the known mirror bug)
    b"/F1 -20 Tf\n"             # negative font size (Y flip within text space)
    b"50 Ts\n"                    # ambient TEXT RISE, INSIDE BT, never reset
    b"0.118 0.114 0.114 rg\n"    # dark ambient fill
    b"180 100 Td\n"
    b"0.859 0.816 0.816 rg\n"     # grey local to the edited span
    b"(SHIP TO) Tj\n"           # edited span (grey, risen +50)
    b"0.118 0.114 0.114 rg\n"    # back to dark
    b"0 60 Td (KEEP ME) Tj\n"    # second span keeps the editor on the pass-through
    b"ET\n"                       # path so the injection is emitted INLINE.
    b"Q Q\n" % H
)


def _build(objs):
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


def main():
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 %d] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>" % H,
        b"<< /Length %d >>\nstream\n" % len(CONTENT) + CONTENT + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "state_leak_ts.pdf")
    with open(path, "wb") as f:
        f.write(_build(objs))
    print("wrote", path)


if __name__ == "__main__":
    main()
