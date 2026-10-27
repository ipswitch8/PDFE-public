# -*- coding: utf-8 -*-
"""Generate a simple 100-page PDF test fixture using reportlab."""

from reportlab.pdfgen import canvas

OUTPUT_PATH = r"C:\Users\dev\source\PDFE\tests\fixtures\test_100page.pdf"
PAGE_WIDTH = 612
PAGE_HEIGHT = 792


def generate():
    c = canvas.Canvas(OUTPUT_PATH, pagesize=(PAGE_WIDTH, PAGE_HEIGHT))
    for n in range(1, 101):
        c.setFont("Helvetica", 24)
        text = f"Page {n}"
        text_width = c.stringWidth(text, "Helvetica", 24)
        x = (PAGE_WIDTH - text_width) / 2
        y = PAGE_HEIGHT / 2
        c.drawString(x, y, text)
        c.showPage()
    c.save()
    print(f"Written: {OUTPUT_PATH}")


if __name__ == "__main__":
    generate()
