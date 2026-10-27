#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build manifest.json for the malicious-pdf corpus.

Walks the corpus directory, computes sha256 for each payload, looks up
the upstream description by test ID, classifies each into a coarse
category, and maps each one to the PDFE attack surface it probes.

Output schema (one record per payload):

    {
      "relative_path": "test1.pdf",
      "category": "uri" | "launch" | "submitform" | "embedded_file"
                | "xfa" | "openaction" | "js" | "uri" | "other",
      "upstream_payload_id": "1" | "33_4" | ...,
      "sha256": "<hex>",
      "expected_pdfe_behavior":
        "open_clean" | "prompt_user" | "refuse" | "sandbox_block",
      "maps_to_pdfe_surface": "<free text>"
    }

Run from gen_malicious_corpus.ps1; standalone invocation:
    python scripts/security/build_manifest.py --corpus tests/security/corpus/malicious_pdf \
                                              --out    tests/security/corpus/manifest.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

# Per-payload description + classification, sourced from upstream's
# pdf_descriptions table at malicious-pdf SHA c33050c637c75dfd9bea8afcabad0f41a928c070.
# Each entry is (description, category, expected_pdfe_behavior, maps_to_pdfe_surface).
#
# Categories are coarse and chosen to map to PDFE feature areas:
#   js           - any JavaScript execution path (MuJS engine)
#   launch       - /Launch action (must prompt; viewer never auto-executes)
#   submitform   - /SubmitForm or this.submitForm (must hit HostAllowlist)
#   uri          - /URI action (QDesktopServices::openUrl path)
#   goto         - /GoTo / /GoToR / /GoToE (page navigation, FileSpec)
#   xfa          - XFA forms (PDFE has no XFA renderer; pure parser surface)
#   embedded     - /EmbeddedFiles or /AF associated files
#   eicar        - antivirus test pattern (host AV may quarantine)
#   svg_polyglot - non-PDF that PDFE shouldn't try to open
#   uri_unc      - UNC-form URI for NTLM relay
#   xxe          - XML external entity (XMP / XFA)
#   font         - malicious font / FontMatrix
#   other        - catch-all
#
# expected_pdfe_behavior values:
#   open_clean    PDFE opens the file without any callback or prompt
#   prompt_user   PDFE shows a confirmation dialog (e.g. /Launch)
#   refuse        PDFE rejects the file (parse error or refusal)
#   sandbox_block phase-3 firewall + listener catches a callback that
#                 would have escaped if mitigations were missing

PDF_TABLE: dict[str, tuple[str, str, str, str]] = {
    "1":     ("CVE-2018-4993 /GoToE UNC NTLM callback",        "goto",         "open_clean",   "MuPDF parses /Annots /A /S /GoToE; PDFE never resolves UNC FileSpec for /GoToE"),
    "1.1":   ("CVE-2018-4993 /GoToE HTTPS URL",                "goto",         "open_clean",   "Same as 1; HTTPS URL FileSpec, PDFE only follows /GoTo within document"),
    "2":     ("XFA submit XDP exfiltrates form data",          "xfa",          "open_clean",   "PDFE has no XFA renderer; XFA stream parsed by MuPDF only, no submit action triggered"),
    "3":     ("/OpenAction app.openDoc() remote URL",          "js",           "open_clean",   "JavaScriptEngine (MuJS) sandbox; app.openDoc not implemented; OpenAction JS allowed but cannot exfiltrate"),
    "4":     ("CVE-2019-7089 XFA external XSLT UNC callback",  "xfa",          "open_clean",   "Same XFA story as 2; XSLT external resolution disabled"),
    "5":     ("/URI action triggers DNS/HTTP",                 "uri",          "open_clean",   "URI from /Annots /A is shown but only followed via canvas click + followLinkAt -> QDesktopServices::openUrl; auto-trigger via OpenAction /S /URI is not honored"),
    "6":     ("/Launch action with external URL",              "launch",       "prompt_user",  "MainWindow::followLinkAt -> Launch case shows QMessageBox confirmation before any shell invocation"),
    "7":     ("/GoToR remote PDF",                             "goto",         "open_clean",   "PDFE does not follow /GoToR; FileSpec lookup is local-only"),
    "8":     ("/SubmitForm with HTML flags",                   "submitform",   "sandbox_block","FormSubmitter goes through HostAllowlist; non-loopback target should be refused unless host explicitly allowed"),
    "9":     ("/ImportData external data import",              "submitform",   "open_clean",   "Not wired in PDFE viewer; FormDataIO is for explicit user import only"),
    "10":    ("CVE-2017-10951 Foxit this.getURL JS callback",  "js",           "open_clean",   "MuJS this.getURL not implemented; calling it raises a JS error that is swallowed"),
    "11":    ("EICAR test pattern",                            "eicar",        "open_clean",   "EICAR is plain text inside a PDF stream; not a PDF feature; host AV may quarantine before PDFE sees it"),
    "12":    ("CVE-2014-8453 XFA FormCalc Post()",             "xfa",          "open_clean",   "No XFA FormCalc engine in PDFE"),
    "13":    ("XFA submit CRLF injection",                     "xfa",          "open_clean",   "No XFA renderer"),
    "14":    ("ImageMagick SVG/MSL polyglot",                  "svg_polyglot", "refuse",       "Not a PDF; PDFE refuses non-PDF inputs"),
    "15":    ("FormCalc Post() arbitrary headers",             "xfa",          "open_clean",   "No XFA"),
    "16":    ("/GoToE javascript: URI XSS",                    "goto",         "open_clean",   "PDFE never invokes javascript: scheme externally; QDesktopServices is only reached for explicit user link clicks and javascript: is filtered"),
    "17":    ("CVE-2014-8452 XMLData.parse XXE",               "xxe",          "open_clean",   "MuPDF's XML parser disables external entity resolution"),
    "18":    ("Unescaped parens duplicate /A JS action",       "js",           "open_clean",   "PDFE does not auto-fire /A JS at OpenAction time; only on explicit user-triggered events"),
    "19":    ("/AA /PV Screen annotation auto-fires JS",       "js",           "open_clean",   "MuJS sandbox; Screen annotations not auto-played; /AA /PV ignored"),
    "20":    ("/AA /PC page-close JS",                         "js",           "open_clean",   "Page-close events not wired to the JS engine"),
    "21":    ("/SubmitForm Flags 256 (entire PDF)",            "submitform",   "sandbox_block","HostAllowlist must reject; if allowed, exfiltration would still go through QNAM and get blocked by phase-3 firewall"),
    "22":    ("this.submitForm() cSubmitAs PDF",               "submitform",   "sandbox_block","HostAllowlist; same as 21"),
    "23":    ("Invisible /Btn widget JS click",                "js",           "open_clean",   "MuJS for form events; clicks on hidden widgets do not fire automatically"),
    "24":    ("/Tx widget submitForm() POST",                  "submitform",   "sandbox_block","HostAllowlist; phase-3 firewall is the second line"),
    "25":    ("getPageNthWord() rendered text exfil",          "js",           "open_clean",   "getPageNthWord returns to JS context only; no network exit"),
    "26":    ("/AA /E mouseover JS",                           "js",           "open_clean",   "Mouseover not wired to JS engine in PDFE viewer"),
    "28":    ("Unescaped parens /URI hijack",                  "uri",          "open_clean",   "URI parsing in MuPDF; PDFE follows only via explicit click"),
    "29":    ("CVE-2024-4367 Type1 FontMatrix break out",      "font",         "open_clean",   "PDF.js-specific; PDFE uses MuPDF + FreeType, not affected"),
    "30":    ("External XObject /FS /URL silent fetch",        "uri",          "open_clean",   "XObject streams are local-only in MuPDF; external /FS /URL is not resolved"),
    "31":    ("/S /Thread remote FileSpec",                    "goto",         "open_clean",   "Thread navigation not implemented in PDFE"),
    "32":    ("/Launch /Win /O /print remote fetch",           "launch",       "prompt_user",  "MainWindow::followLinkAt /Launch confirmation prompt; user must explicitly accept"),
    "33_1":  ("Acrobat JS this.submitForm",                    "js",           "sandbox_block","Goes through HostAllowlist when reached"),
    "33_2":  ("Acrobat JS this.getURL",                        "js",           "open_clean",   "Not implemented in MuJS"),
    "33_3":  ("Acrobat JS app.launchURL",                      "js",           "open_clean",   "Not implemented in MuJS"),
    "33_4":  ("Acrobat JS app.media.getURLData",               "js",           "open_clean",   "Not implemented (no media subsystem)"),
    "33_5":  ("Acrobat JS SOAP.connect",                       "js",           "open_clean",   "No SOAP runtime"),
    "33_6":  ("Acrobat JS SOAP.request",                       "js",           "open_clean",   "No SOAP runtime"),
    "33_7":  ("Acrobat JS this.importDataObject",              "js",           "open_clean",   "Not implemented"),
    "33_8":  ("Acrobat JS app.openDoc",                        "js",           "open_clean",   "Not implemented"),
    "33_9":  ("PDF.js fetch()",                                "js",           "open_clean",   "MuJS lacks fetch()"),
    "33_10": ("PDF.js XMLHttpRequest",                         "js",           "open_clean",   "MuJS lacks XHR"),
    "33_11": ("PDF.js new Image() callback",                   "js",           "open_clean",   "MuJS lacks Image()"),
    "33_12": ("PDF.js WebSocket",                              "js",           "open_clean",   "MuJS lacks WebSocket"),
    "33_13": ("Adobe RSS.addFeed callback",                    "js",           "open_clean",   "Not implemented"),
    "33_14": ("Adobe util.readFileIntoStream + SOAP chain",    "js",           "open_clean",   "Neither piece implemented"),
    "33_15": ("Adobe base64 staged JS via /Tx /V getField",    "js",           "open_clean",   "getField returns to JS context only; no exec primitive"),
    "34_1":  ("UNC image XObject (NTLM via render)",           "uri_unc",      "open_clean",   "MuPDF does not resolve UNC paths in image XObjects on Windows"),
    "34_2":  ("UNC /GoToR FileSpec",                           "goto",         "open_clean",   "Not followed"),
    "34_3":  ("UNC /Thread FileSpec",                          "goto",         "open_clean",   "Not implemented"),
    "34_4":  ("UNC /URI",                                      "uri_unc",      "open_clean",   "Not auto-followed"),
    "34_5":  ("UNC this.submitForm",                           "submitform",   "sandbox_block","HostAllowlist"),
    "34_6":  ("UNC this.getURL",                               "js",           "open_clean",   "Not implemented"),
    "34_7":  ("UNC app.launchURL",                             "js",           "open_clean",   "Not implemented"),
    "34_8":  ("UNC SOAP.connect",                              "js",           "open_clean",   "Not implemented"),
    "34_9":  ("UNC app.openDoc",                               "js",           "open_clean",   "Not implemented"),
    "35":    ("/Names /JavaScript catalog auto-execute",       "js",           "open_clean",   "MuJS runs but cannot exfiltrate; OpenAction JS not auto-fired in PDFE"),
    "36":    ("CVE-2016-2175/2017-9096 XMP XXE",               "xxe",          "open_clean",   "MuPDF XML parser disables external entities"),
    "37":    ("XXE in /AcroForm /XFA stream",                  "xxe",          "open_clean",   "Same"),
    "38":    ("CVE-2020-29075 /AA /WC /WS /DS DNS callback",   "js",           "open_clean",   "Document-level events not wired"),
    "39":    ("CVE-2022-28244 RichMedia HTML/JS CSP bypass",   "embedded",     "open_clean",   "RichMedia not played; annotation read but inert"),
    "40":    ("CVE-2018-5158 /FunctionType 4 PostScript JS",   "font",         "open_clean",   "PDF.js-specific"),
    "41":    ("CVE-2018-20065 /OpenAction /S /URI",            "uri",          "open_clean",   "OpenAction not auto-fired"),
    "42":    ("CVE-2025-66516 OOB %xxe XFA",                   "xxe",          "open_clean",   "No XFA"),
    "43":    ("CVE-2025-70401 <img> Text annot XSS",           "js",           "open_clean",   "Apryse-specific; PDFE renders annotations as image, not HTML"),
    "44":    ("CVE-2024-12426 vnd.sun.star.expand env leak",   "uri",          "open_clean",   "LibreOffice-specific URI scheme; PDFE does not handle"),
    "45":    ("CVE-2025-59803 OCG /AA /WP /DP signing JS",     "js",           "open_clean",   "Foxit-specific; OCG /AA not wired in PDFE"),
    "46":    ("CVE-2026-25755 jsPDF addJS /AA /O auto",        "js",           "open_clean",   "jsPDF-specific writer; PDFE only reads"),
    "47":    ("PDF 2.0 /AF Associated Files HTML/JS",          "embedded",     "open_clean",   "AttachmentManager extracts only on explicit user action"),
    "48":    ("XFA SOAP submit initialize event",              "xfa",          "open_clean",   "No XFA"),
}


def parse_test_id(filename: str) -> str | None:
    """Extract upstream payload id from a filename.
    test1.pdf -> '1'
    test1_1.pdf -> '1.1'
    test33_4.pdf -> '33_4'
    test1.svg / test1.xml -> '1'
    """
    m = re.match(r'^test(\d+)(?:_(\d+))?\.(?:pdf|svg|xml)$', filename)
    if not m:
        return None
    main = m.group(1)
    sub = m.group(2)
    if sub is None:
        return main
    # Disambiguate 1.1 (single decimal) vs 33_1..33_15 (subpayload).
    # Upstream uses 1.1 for the second variant of test 1, and 33_N
    # for the JS family. Heuristic: if main is in {1}, treat as decimal;
    # else as subpayload.
    if main == '1' and sub == '1':
        return '1.1'
    return f'{main}_{sub}'


def sha256_of(p: Path) -> str:
    h = hashlib.sha256()
    with p.open('rb') as f:
        for chunk in iter(lambda: f.read(65536), b''):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--corpus', required=True,
                    help='Path to tests/security/corpus/malicious_pdf/')
    ap.add_argument('--out', required=True,
                    help='Output manifest.json path')
    args = ap.parse_args()

    corpus = Path(args.corpus)
    if not corpus.is_dir():
        print(f'corpus dir not found: {corpus}')
        return 2

    # Phase 2 AC #3 requires
    #   tests/security/corpus/malicious_pdf/<category>/<name>.<ext>
    # Upstream emits flat files (test1.pdf, test2.pdf, ...) so we
    # post-process: move each generated file into its category subdir
    # and emit relative_path as "<category>/<name>" in manifest.json.

    records = []
    unmapped = []
    flat_files = [p for p in sorted(corpus.iterdir())
                  if p.is_file()
                  and p.suffix in ('.pdf', '.svg', '.xml')
                  and p.name != 'SOURCE.txt']
    # Also pick up any files already-organized into category subdirs
    # from a prior run, so the layout converges on rerun.
    for sub in sorted([d for d in corpus.iterdir() if d.is_dir()]):
        for p in sorted(sub.iterdir()):
            if p.is_file() and p.suffix in ('.pdf', '.svg', '.xml'):
                flat_files.append(p)

    for p in flat_files:
        tid = parse_test_id(p.name)
        if tid is None or tid not in PDF_TABLE:
            unmapped.append(str(p.relative_to(corpus)))
            continue
        desc, category, expected, surface = PDF_TABLE[tid]
        target_dir = corpus / category
        target_dir.mkdir(parents=True, exist_ok=True)
        target = target_dir / p.name
        if p != target:
            if target.exists():
                target.unlink()
            p.replace(target)
            p = target
        records.append({
            'relative_path':           f'{category}/{p.name}',
            'category':                category,
            'upstream_payload_id':     tid,
            'description':             desc,
            'sha256':                  sha256_of(p),
            'expected_pdfe_behavior':  expected,
            'maps_to_pdfe_surface':    surface,
        })

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({
        'schema_version': 1,
        'corpus_size': len(records),
        'unmapped_files': unmapped,
        'payloads': records,
    }, indent=2), encoding='utf-8')

    print(f'wrote {out} ({len(records)} payloads, {len(unmapped)} unmapped)')
    if unmapped:
        for u in unmapped:
            print(f'  unmapped: {u}')
    return 0 if not unmapped else 1


if __name__ == '__main__':
    raise SystemExit(main())
