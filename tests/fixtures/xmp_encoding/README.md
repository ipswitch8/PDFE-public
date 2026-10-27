# XMP encoding fixture corpus — provenance

Meta-audit 2026-07 follow-up, **phase F36**.
Added 2026-08-07. Consumed by `tests/test_xmp_encoding_utf16.cpp`.

| file | encoding | leading BOM | bytes |
|---|---|---|---|
| `xmp_pdfa2b_utf8.xmp` | UTF-8 | `EF BB BF` | 807 |
| `xmp_pdfa2b_utf16le.xmp` | UTF-16LE | `FF FE` | 1598 |
| `xmp_pdfa2b_utf16be.xmp` | UTF-16BE | `FE FF` | 1598 |

## Provenance

**Synthetic — generated, not harvested.** Produced by
`generate_xmp_encoding_fixtures.py` in this directory, which encodes ONE source
string three ways. No third-party document, no vendor sample, no copyrighted
input: the packet was written by hand for this phase. Re-running the generator
reproduces the three files byte-for-byte.

The packet is a PDF/A-2b identification packet in the ISO 16684-1 form, with
`pdfaid:part` / `pdfaid:conformance` as **child elements** (the branch
`XmpMetadataWriter::detectPdfALevel` takes for packets PDFE did not itself
write), plus `dc:title`, `xmp:CreatorTool` and `pdf:Producer`. The title
carries U+00DC, U+00EF and the **astral** U+1D11E so a surrogate-pair
transcoding bug cannot hide behind an all-ASCII fixture.

ISO 16684-1 puts a U+FEFF inside the `<?xpacket begin="…"?>` header, encoded in
the packet's own encoding. These fixtures carry that **and** a leading BOM at
offset 0, because acceptance criterion 1 of phase F36 asks specifically for
"UTF-16LE/BE with BOM". The realistic no-leading-BOM form (where the first
bytes are `3C 00 3F 00`, not `FF FE`) is derived **in the test** by dropping the
first two bytes, so it needs no fourth file and cannot drift from these.

## Why these files are `binary` in `.gitattributes`

`core.autocrlf=true` is set on this repo, so `.gitattributes` carries
`*.xmp binary`.

**Which file that rule actually saves is not the one you would guess, and it
was measured rather than assumed** — see
`../../../docs/audit/meta-audit-2026-07/followup/phase-36/gitattributes-checkout-probe.md`,
which commits the three files to two throwaway repos (one with the rule, one
without) and checks them back out:

| file | no rule | with rule |
|---|---|---|
| `xmp_pdfa2b_utf16be.xmp` | 1598 — survives | 1598 |
| `xmp_pdfa2b_utf16le.xmp` | 1598 — survives | 1598 |
| `xmp_pdfa2b_utf8.xmp` | **827 — corrupted** (was 807) | 807 |

The UTF-16 files survive without the rule because git's clean/smudge code
treats a blob containing NUL bytes as binary and skips CRLF conversion, and
every other byte of a UTF-16 file is NUL. It is the **UTF-8** member that gets
rewritten — 20 LFs into CRLFs — and that breaks the transcode-set property just
as thoroughly as mangling a UTF-16 file would. So the rule is load-bearing for
the UTF-8 file, and for the UTF-16 files it replaces a content *heuristic* with
an explicit guarantee that a future `*.xmp text` or `text=auto` rule cannot
silently override.

That rule is a control, not a promise, so the corpus is **self-checking**:
`XmpEncodingTest.FixtureCorpusIsAnExactTranscodeSet` decodes all three files
and asserts they yield the identical string. If any one of them is line-ending
mangled on checkout, the three stop agreeing and that test fails — loudly, and
in every future run, not only in the session that wrote them.
