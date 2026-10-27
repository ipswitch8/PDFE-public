# Windows desktop-integration limitations

<!-- tracking-banner -->
> **STATUS: LIVE OPERATIONAL STATE — keep maintaining this file.**
>
> This is **not** a defect tracker and is **not** superseded by GitHub Issues;
> it is state that code, CI or a fixture generator reads. **GitHub Issues
> (`ipswitch8/PDFE`) remains the canonical tracker for anything still open** — a
> defect recorded only here is still an untracked defect.
>
> See [`docs/TRACKING.md`](TRACKING.md).

---


Recorded limitations in PDFE's Windows shell / desktop integration that are
accepted rather than fixed, with the reasoning and the trigger that would
reopen each one. Machine-checked by `tests/test_update_channel_docs.cpp`
(the `WindowsIntegrationLimitations` suite) so an entry cannot silently
disappear.

---

## UIPI-LIMITATION-ELEVATED-DRAGDROP — drag-and-drop into an elevated PDFE

<!-- UIPI-DRAGDROP-STATUS: DOCUMENTED-LIMITATION -->

**The behaviour.** If PDFE is running elevated (as Administrator) and Explorer
is running at the normal integrity level — the default configuration on any
machine where a user right-clicks "Run as administrator" — dragging a file from
Explorer onto the PDFE window does nothing. No error, no cursor feedback, no
diagnostic.

**Why.** This is User Interface Privilege Isolation, not a PDFE defect. Windows
blocks window messages sent from a lower-integrity process to a
higher-integrity one. Drag-and-drop needs `WM_DROPFILES` and
`WM_COPYGLOBALDATA`, both of which are filtered out. `grep -rn
'ChangeWindowMessageFilterEx' src` returns 0 hits: PDFE does not lower the
filter.

**Decision: document, do not filter.** Calling
`ChangeWindowMessageFilterEx(hwnd, WM_COPYGLOBALDATA, MSGFLT_ALLOW, ...)` would
restore the drop, but it deliberately punches a hole in an integrity boundary
that exists to stop a lower-privileged process feeding data to a
higher-privileged one — for an editor that parses hostile, attacker-supplied
file formats, in a process that has just been handed Administrator. The
cost/benefit does not justify it:

* PDFE has **no reason to run elevated**. It writes to user-owned documents and
  `HKCU`; installation (the only step that touches `HKLM`) is the installer's
  job, not the editor's.
* The workaround is complete and immediate: **File → Open**, the recent-files
  list, the command line, and double-click from Explorer (which launches a
  *new*, non-elevated process) all work normally.

**What would reopen this.** A supported scenario in which PDFE must run
elevated. If one appears, the fix is not a blanket message filter but the
narrowest possible allow-list on the drop target window only, paired with a test
that an elevated instance still refuses malformed payloads.

**Not covered by an automated test, on purpose.** Reproducing it requires
launching PDFE elevated, which needs Administrator rights and a UAC consent the
test harness cannot supply; and a test that merely asserts "no drop happened"
would pass equally on a broken drop handler. The limitation is recorded here
instead, which is the disposition the gap register's own remedy allows.

---

## CLIPBOARD-OWNER-LIFETIME — copied images do not outlive the process

<!-- CLIPBOARD-LIFETIME-STATUS: OPEN-DEFERRED -->

**The behaviour (unverified, tracked as gap WIN-24).** Qt renders clipboard
formats lazily through OLE delayed rendering and does not call
`OleFlushClipboard` at shutdown (`grep -rn 'OleFlushClipboard' src tests` → 0
hits). If a user copies a page region in PDFE and then quits PDFE before
pasting, the clipboard content may be lost.

**Status: open, not accepted.** Unlike the UIPI entry above, this is a probable
product defect rather than an OS constraint, and the fix is small (flush on
`QCoreApplication::aboutToQuit`). It is recorded here so it is not mistaken for
an accepted limitation. Closing it requires a cross-process test: copy in a
child PDFE, terminate that child, then read the Windows clipboard from the
surviving test process.

---

## Cross-application clipboard formats — covered, not a limitation

For the avoidance of doubt: PDFE's copy-out **does** publish the
cross-application bitmap formats (`CF_DIB`, `CF_BITMAP`). This is asserted
against the Windows clipboard API — not through `QClipboard` — by
`tests/test_clipboard_native_formats.cpp`.
