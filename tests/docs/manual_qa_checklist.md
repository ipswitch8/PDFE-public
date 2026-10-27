# PDFE Manual QA Checklist

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what was found or decided on the day it was
> written. Items here may have been fixed, superseded, or deliberately declined
> since. Where this document and a GitHub issue disagree, **the issue wins**.
>
> See [`docs/TRACKING.md`](../../docs/TRACKING.md).

---


**Purpose:** Pre-release verification of items that automated tests cannot
adequately cover. To be executed by a human tester before each release.

**Frequency:** Every release candidate. Estimated time: 4-6 hours full
walk-through; 1.5 hours smoke pass.

**Sign-off:** A release manager must check off each section before the
build is approved for distribution.

---

## How to Use This Checklist

Each item has:
- **Action:** Exactly what to do
- **Expected:** What should happen
- **Failure indicator:** What constitutes a defect
- **Pass / Fail / N/A** column to fill in

When a defect is found, file a ticket referencing the section ID and
attach a screen capture + repro steps.

---

## Section A — Printing (Real Hardware)

### A1. Print to physical printer
- **Action:** Open `tests/fixtures/test_3page.pdf`. File → Print. Select a
  real physical printer (not "Microsoft Print to PDF"). Click Print.
- **Expected:** All 3 pages emerge correctly, in order, with text legible.
- **Failure indicator:** Missing pages, garbled text, ink/toner anomalies
  attributable to PDFE output (not the printer driver itself).
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### A2. Print scaling — Fit to page
- **Action:** Open a Letter-size PDF. File → Print. Set "Scale: Fit to
  printable area." Print to A4 paper.
- **Expected:** Content scales down; no clipping.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### A3. Duplex printing
- **Action:** Print a 4-page PDF in duplex mode.
- **Expected:** Pages 1+2 on first sheet (front/back), 3+4 on second.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### A4. Print range
- **Action:** Print pages 2-3 of a 5-page document.
- **Expected:** Only pages 2 and 3 print.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### A5. Print encrypted PDF (no-print permission)
- **Action:** Open `test_encrypted_restricted.pdf` (no-print permission).
  File → Print.
- **Expected:** Print action disabled, or user warned, or output blank.
- **Failure indicator:** PDF prints normally despite restriction.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

**Section A sign-off:** ☐ Approved by: __________ Date: __________

---

## Section B — Digital Signatures (Real Cryptographic Hardware)

### B1. Sign with USB eToken / smart card
- **Action:** Insert SafeNet eToken (or equivalent USB cryptographic
  token). Open an unsigned PDF. Security → Sign Document. Select cert
  from the token. Enter PIN. Place signature on canvas. Click Sign.
- **Expected:** Signature dialog detects the eToken cert. PIN prompt
  appears. Signed PDF saves. Reopening shows valid signature with
  trusted chain.
- **Failure indicator:** eToken not detected, PIN prompt missing,
  signature invalid on reopen.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### B2. Verify Acrobat-signed PDF
- **Action:** Have a colleague sign a PDF in Adobe Acrobat with a real
  cert (e.g., from DigiCert or Entrust). Open in PDFE.
- **Expected:** Signature panel shows "Valid signature, trusted chain."
- **Failure indicator:** PDFE reports invalid or untrusted when Acrobat
  shows valid.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### B3. Acrobat verifies PDFE-signed PDF
- **Action:** Sign a PDF in PDFE with `test_signer.pfx`. Open the result
  in Adobe Acrobat.
- **Expected:** Acrobat displays signature info correctly (signer name,
  reason, date). Acrobat may report untrusted chain (test cert is
  self-signed) but should NOT report tampering.
- **Failure indicator:** Acrobat reports "document modified after signing."
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### B4. Timestamp authority — real TSA
- **Action:** Sign with TSA URL set to a real public TSA (e.g.,
  `http://timestamp.digicert.com`). Verify network call goes out.
- **Expected:** Signed PDF contains valid timestamp token; signature
  panel shows TSA time.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

**Section B sign-off:** ☐ Approved by: __________ Date: __________

---

## Section C — Accessibility (Real Assistive Technology)

### C1. Screen reader — NVDA announces menus
- **Action:** Launch NVDA. Open PDFE. Press Alt to focus menu bar.
  Arrow through File menu items.
- **Expected:** NVDA reads each menu item ("File menu", "Open",
  "Recent files", etc.).
- **Failure indicator:** Silent menus, "unlabeled" announcements,
  wrong order.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### C2. Screen reader — JAWS reads dialog
- **Action:** With JAWS running, open Preferences dialog.
  Tab through fields.
- **Expected:** JAWS announces each control with its label and current
  value.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### C3. Screen reader — NVDA reads PDF content
- **Action:** Open a tagged PDF (e.g., one generated by Microsoft Word
  with "Document properties" → "Save document structure tags for
  accessibility"). Use NVDA browse mode.
- **Expected:** NVDA reads paragraphs, headings, links in correct order.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### C4. Keyboard-only navigation — Preferences
- **Action:** Open Preferences with Ctrl+, or via menu. Without using
  the mouse, navigate to each tab and modify one setting on each.
  Save and close.
- **Expected:** Tab order is logical. Arrow keys move within tab list.
  Mnemonics (Alt+letter) work. Enter activates default button.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### C5. High-contrast theme
- **Action:** Windows Settings → Accessibility → Contrast themes → Aquatic
  (or similar high-contrast theme). Restart PDFE.
- **Expected:** PDFE respects system theme. All text remains readable.
- **Failure indicator:** Hardcoded colors make text unreadable.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

**Section C sign-off:** ☐ Approved by: __________ Date: __________

---

## Section D — Multi-Monitor / High-DPI

### D1. Drag between monitors of different DPI
- **Action:** Connect a 4K (192 DPI) monitor alongside a 1080p (96 DPI)
  monitor. Launch PDFE on the 96-DPI monitor. Drag window to the 4K
  monitor.
- **Expected:** Widgets scale up; text remains crisp; no overflow.
- **Failure indicator:** Blurry text, clipped widgets, misaligned panels.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### D2. Open dialog inherits parent monitor
- **Action:** With PDFE on the 4K monitor, open File → Preferences.
- **Expected:** Preferences dialog appears on the same monitor as
  the main window, at the correct DPI scale.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### D3. Mid-session DPI change
- **Action:** While PDFE is running, connect/disconnect a monitor or
  change the active monitor's scaling in Windows Settings.
- **Expected:** PDFE adapts within ~2 seconds. No window relaunch needed.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

**Section D sign-off:** ☐ Approved by: __________ Date: __________

---

## Section E — Real-World Document Interop

### E1. IRS form filling
- **Action:** Download IRS Form W-9 (interactive PDF). Open in PDFE.
  Fill name, address, SSN, signature, date. Save.
- **Expected:** Tab order correct, calculated fields update, save preserves
  data. Reopen in Acrobat — all fields populated.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### E2. Word-exported PDF
- **Action:** In Microsoft Word, create a 5-page document with a TOC,
  table, image, hyperlink. Export to PDF. Open in PDFE.
- **Expected:** Layout matches Word, hyperlinks clickable, image visible.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### E3. InDesign-exported PDF
- **Action:** Open a PDF exported from Adobe InDesign with complex
  typography (kerning, ligatures, justified text).
- **Expected:** Typography renders correctly, matches InDesign preview.
- **Failure indicator:** Wrong font metrics, missing glyphs.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### E4. Scanned 100+ page archive
- **Action:** Open a real scanned PDF (e.g., from a document scanner)
  with 100+ pages. Activate TRE OCR. Edit a recognized word.
- **Expected:** OCR completes, edit is applied, save preserves change.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### E5. Multi-language content
- **Action:** Open a PDF containing English, Chinese (CJK), Arabic (RTL),
  and a math equation. Render and search.
- **Expected:** All scripts render correctly; CJK fonts substitute;
  RTL flows right-to-left.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

**Section E sign-off:** ☐ Approved by: __________ Date: __________

---

## Section F — Long-Session Stability

### F1. 8-hour soak test
- **Action:** Open PDFE in the morning. Throughout an 8-hour work day,
  use it normally: open documents, edit, save, close, repeat. At end
  of day, check Task Manager.
- **Expected:** Memory consumption < 500 MB. No "PDFE has stopped
  responding" dialogs.
- **Failure indicator:** RSS > 1 GB, handle count > 5000, UI sluggish.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### F2. Rapid open-close cycling
- **Action:** Open and close 50 different PDFs in rapid succession
  (~20 seconds each).
- **Expected:** No memory growth between cycles; no crash; UI remains
  responsive.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

**Section F sign-off:** ☐ Approved by: __________ Date: __________

---

## Section G — Recovery and Error Handling

### G1. Disk full during save
- **Action:** Edit a PDF. Fill the disk to within 1 MB of capacity.
  Attempt to save the PDF.
- **Expected:** Clear error message ("Disk full"). PDFE does not crash.
  Original file not corrupted.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### G2. Network drive disconnect during save
- **Action:** Open a PDF from a network share. Edit. Disconnect the
  network share (unplug ethernet or kill the VPN). Attempt to save.
- **Expected:** Clear error message. PDFE does not hang or crash.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### G3. Corrupted PDF mid-render
- **Action:** Open a PDF, scroll past page 1, then replace the underlying
  file with a corrupted version via external tool.
- **Expected:** PDFE either reloads cleanly, shows an error, or continues
  with cached pages. Does not crash.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

### G4. Permission denied
- **Action:** Attempt to open a file that the user has no permission to
  read.
- **Expected:** Clear error message; PDFE continues normally.
- **Result:** ☐ Pass ☐ Fail ☐ N/A

**Section G sign-off:** ☐ Approved by: __________ Date: __________

---

## Section H — Smoke Pass (15-minute version)

For frequent verification (e.g., daily builds), execute only:
- A1 (one print job to real printer)
- B2 (Acrobat-signed PDF verification)
- C1 (NVDA reads menu)
- D1 (drag between monitors)
- E1 (IRS form fill)
- F2 (50 open-close cycles)

**Smoke pass result:** ☐ Pass ☐ Fail
**Date:** __________  **Tester:** __________

---

## Release Sign-Off

By signing below, the release manager confirms that all applicable
sections of this checklist have been executed and that any failures
have been documented with ticket references and approved by the
appropriate stakeholder for inclusion in this release.

| Section | Status | Ticket(s) for failures | Approver |
|---|---|---|---|
| A (Printing) | ☐ Pass ☐ Fail | | |
| B (Signatures) | ☐ Pass ☐ Fail | | |
| C (Accessibility) | ☐ Pass ☐ Fail | | |
| D (Multi-monitor) | ☐ Pass ☐ Fail | | |
| E (Real-world docs) | ☐ Pass ☐ Fail | | |
| F (Long session) | ☐ Pass ☐ Fail | | |
| G (Error recovery) | ☐ Pass ☐ Fail | | |

**Release manager:** ____________________
**Release version:** _________
**Build hash:** ______________
**Date:** ___________

**Final approval:** ☐ Release approved ☐ Release blocked
