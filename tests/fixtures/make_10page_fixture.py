# -*- coding: utf-8 -*-
"""Pipeline E phase-11: tiny 10-page fixture used by the combine-files
perf measurement (AC: combining 10 × 10-page PDFs in < 3 s)."""
from pathlib import Path
import pikepdf

HERE = Path(__file__).parent
OUT = HERE / "test_10page.pdf"


def main() -> None:
    pdf = pikepdf.Pdf.new()
    for _ in range(10):
        pdf.add_blank_page(page_size=(612, 792))
    pdf.save(str(OUT))
    print(f"wrote {OUT} ({len(pdf.pages)} pages)")


if __name__ == "__main__":
    main()
