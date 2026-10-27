#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate a minimal PDF portfolio (package) for testing.

A portfolio PDF has:
- A /Collection dict in /Catalog
- Embedded files in /Names /EmbeddedFiles
- Optional /View entry on /Collection (D=details, T=tile, H=hidden)

This writes a skeletal portfolio with two dummy embedded files and a
/Collection /View D entry so AttachmentManager.isPortfolio() /
portfolioView() have something to read.
"""

from pathlib import Path


def make_portfolio_pdf(out_path: Path) -> None:
    """Write a minimal valid portfolio PDF by hand (no external deps)."""
    # Object ids:
    #   1 = Catalog (with /Collection)
    #   2 = Pages
    #   3 = single blank page
    #   4 = Collection dict
    #   5 = Names
    #   6 = EmbeddedFiles name tree
    #   7 = Filespec for "a.txt"
    #   8 = Embedded file stream for "a.txt"
    #   9 = Filespec for "b.txt"
    #  10 = Embedded file stream for "b.txt"
    objs: list[bytes] = [b""]

    def add(body: bytes) -> int:
        objs.append(body)
        return len(objs) - 1

    add(
        b"<< /Type /Catalog /Pages 2 0 R /Collection 4 0 R "
        b"/Names 5 0 R >>"
    )
    add(b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    add(
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << >> >>"
    )
    add(b"<< /Type /Collection /View /D >>")
    add(b"<< /EmbeddedFiles 6 0 R >>")
    add(
        b"<< /Names [ (a.txt) 7 0 R (b.txt) 9 0 R ] >>"
    )
    add(
        b"<< /Type /Filespec /F (a.txt) /UF (a.txt) "
        b"/EF << /F 8 0 R >> >>"
    )
    add(
        b"<< /Type /EmbeddedFile /Length 5 >>\nstream\nhello\nendstream"
    )
    add(
        b"<< /Type /Filespec /F (b.txt) /UF (b.txt) "
        b"/EF << /F 10 0 R >> >>"
    )
    add(
        b"<< /Type /EmbeddedFile /Length 5 >>\nstream\nworld\nendstream"
    )

    buf = bytearray(b"%PDF-1.5\n%\xff\xff\xff\xff\n")
    offsets = [0]
    for i in range(1, len(objs)):
        offsets.append(len(buf))
        buf += f"{i} 0 obj\n".encode("ascii") + objs[i] + b"\nendobj\n"
    xref_off = len(buf)
    buf += f"xref\n0 {len(objs)}\n".encode("ascii")
    buf += b"0000000000 65535 f \n"
    for off in offsets[1:]:
        buf += f"{off:010d} 00000 n \n".encode("ascii")
    buf += (
        b"trailer\n<< /Size "
        + str(len(objs)).encode("ascii")
        + b" /Root 1 0 R >>\nstartxref\n"
        + str(xref_off).encode("ascii")
        + b"\n%%EOF\n"
    )
    out_path.write_bytes(bytes(buf))


def main() -> None:
    out = Path(__file__).parent / "test_portfolio.pdf"
    make_portfolio_pdf(out)
    print(f"wrote {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
