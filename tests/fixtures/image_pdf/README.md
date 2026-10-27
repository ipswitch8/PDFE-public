# Image-based PDF fixtures

Synthetic test PDFs for the image-based PDF edit pipeline risks
documented in `docs/image-pdf-risks.md`. The fixtures themselves are
**not committed** (they total ~60 MB). Regenerate them with:

```powershell
cd tests/fixtures/image_pdf
python gen_fixtures.py
```

Requires: `pymupdf`, `Pillow`. All three are in the PDFE dev
environment.

Generation is deterministic (seeded RNG, no timestamps) so the same
bytes come out on every run.

## Fixture matrix

### A3 — source image DPI tracking
Each fixture rasterizes the same sample content at a different DPI
and wraps it in a PDF page whose MediaBox matches the implied
physical size. `img_pixels / (page_points/72) == fixture_dpi`
exactly.

| Fixture | DPI | Page |
|---------|-----|------|
| `a3_72dpi_web_letter.pdf` | 72 | US Letter |
| `a3_150dpi_a4_photo.pdf` | 150 | A4 |
| `a3_200dpi_fax_legal.pdf` | 200 | US Legal |
| `a3_300dpi_office_letter.pdf` | 300 | US Letter |
| `a3_600dpi_archival_a4.pdf` | 600 | A4 |
| `a3_1200dpi_ocr_letter.pdf` | 1200 | US Letter |
| `a3_mixed_dpi_5page.pdf` | 72/150/300/600/1200 | Letter (per-page) |

### C2 — LRU cache capacity
Multi-page fixtures with controlled per-page image size.

| Fixture | Pages | DPI | Size |
|---------|-------|-----|------|
| `c2_small_6page_300dpi.pdf` | 6 | 300 | Letter |
| `c2_large_12page_600dpi.pdf` | 12 | 600 | Letter |
| `c2_tiny_20page_72dpi.pdf` | 20 | 72 | Letter |
| `c2_mixed_8page.pdf` | 8 | 72-1200 | Letter |
| `c2_color_ledger_10page.pdf` | 10 | 300 | Legal |

### B3 — background classification
Different background types at controlled DPIs.

| Fixture | Background | DPI |
|---------|------------|-----|
| `b3_uniform_white_300dpi.pdf` | Clean white | 300 |
| `b3_yellowed_news_300dpi.pdf` | Uniform yellow | 300 |
| `b3_halftone_news_200dpi.pdf` | 50% halftone | 200 |
| `b3_halftone_news_600dpi.pdf` | 50% halftone | 600 |
| `b3_blue_ledger_300dpi.pdf` | Ruled ledger | 300 |
| `b3_textured_gray_300dpi.pdf` | Speckle noise | 300 |
| `b3_gradient_shaded_300dpi.pdf` | Shadow gradient | 300 |
| `b3_highlighted_yellow_300dpi.pdf` | Yellow highlight | 300 |
| `b3_jpeg_blocky_200dpi.pdf` | JPEG blocking | 200 |

## Verification

Run `verify_fixtures.py` to confirm each PDF contains an image
XObject whose pixel dimensions produce the expected DPI:

```powershell
python verify_fixtures.py
```

## Notes

- A3 halftone fixtures exist at both 200 and 600 DPI with the same
  visual pattern (scaled) specifically so tests can assert the
  halftone classifier's kernel sizing is DPI-aware. If the
  classifier passes at one DPI but fails at the other, it's
  coincidentally tuned rather than correct.
- The `a3_mixed_dpi_5page.pdf` and `c2_mixed_8page.pdf` multi-page
  fixtures intentionally mix DPIs across pages so tests can assert
  per-page DPI detection (not a single document-level assumption).
- JPEG fixtures are embedded with `quality=75` or `quality=25`
  (b3_jpeg_blocky) to exercise JPEG-decode paths in C2 cache tests.
