# WinAppDriver Setup Guide (PDFE UI E2E)

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


**Tracks:** PDFE-LIMITATION-WINAPPDRIVER-V0 (see `known_issues.md`).

PDFE's hybrid native-UI tests use **WinAppDriver** to drive the native
Win32 shell file dialogs (`#32770` Open / Save As windows) that
`QFileDialog::getOpenFileName()` / `getSaveFileName()` default to. These
dialogs live outside PDFE's Qt UIA subtree, so pywinauto's in-app driver
cannot reach the file-name edit reliably; WinAppDriver's W3C/JSON-Wire
protocol handles the ComboBox/Edit transition that swallows pywinauto
keystrokes on Windows 10/11.

## Minimum supported version

**WinAppDriver 1.2.1 or later (Sept 2020 GA).**

The 2016 "v0" beta returns `200 OK` on `GET /status` but its
`POST /session` request times out — it cannot create a session at all.
A stale v0 install therefore looks "reachable" to a naive TCP probe yet
fails every test with an opaque timeout. PDFE's test harness guards
against this:

- `tests/ui_automation/helpers/winappdriver_dialog_driver.py` exposes
  `MIN_WINAPPDRIVER_VERSION = (1, 2, 1)`,
  `get_winappdriver_version()`, and `require_winappdriver_version()`.
- Every WAD-driven test calls `require_winappdriver_version()` (directly,
  or via each file's `_skip_if_service_unavailable` /
  `_skip_if_unavailable` helper), which `pytest.skip`s with a clear
  message when the service is unreachable or below 1.2.1.
- `windows-validation.yml` has a **"Verify WinAppDriver version"** step
  that records the installed version (or its absence) in the CI step
  summary before the UI suite runs, so WAD skips are interpretable.

## Install

1. Download the latest release (>= 1.2.1) from
   <https://github.com/microsoft/WinAppDriver/releases>.
   The current GA is **1.2.1** (`WindowsApplicationDriver_1.2.1.msi`).
2. Install to the default location
   `C:\Program Files (x86)\Windows Application Driver\`.
3. Enable **Developer Mode** in Windows Settings
   (Settings → Privacy & security → For developers → Developer Mode).
   WinAppDriver requires it to attach to arbitrary processes.

## Run the service

```powershell
& "C:\Program Files (x86)\Windows Application Driver\WinAppDriver.exe"
```

It listens on `http://127.0.0.1:4723` by default. Override with the
`WINAPPDRIVER_URL` environment variable if you start it on another port.

Leave the service running in its own console; the test harness connects
to it over HTTP and does not start/stop it.

## Run the WAD tests

```powershell
# From the repo root, with PDFE.exe already built:
$env:PDFE_EXE = "build\vs2022\bin\Debug\PDFE.exe"
pytest tests/ui_automation/test_winappdriver_smoke.py -v
pytest tests/ui_automation/test_ui_file_dialog_flows_wad.py -v
pytest tests/ui_automation/test_ui_encryption_flows_wad.py -v
```

When WinAppDriver is **not** running, or is below 1.2.1, these tests
`skip` (they do not fail) — the suite stays green on machines without
the service. This keeps WAD an opt-in layer on top of the always-on
pywinauto/PyAutoGUI coverage.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `POST /session` times out, `/status` is 200 | v0 beta installed | Upgrade to >= 1.2.1 |
| "WinAppDriver service not reachable" skip | service not started | Run `WinAppDriver.exe` |
| Session creates but can't attach to PDFE | Developer Mode off | Enable Developer Mode |
| Orphaned `PDFE.exe` after a run | test bailed before `driver.quit()` | Kill stray `PDFE.exe`; tests call `quit()` in `finally`, but a hard crash can leak one |

## Why opt-in (not a hard CI dependency)

The self-hosted `pdfe-windows` runner does not always have WinAppDriver
installed or Developer Mode enabled, and a running WAD service competes
for the desktop session with the pywinauto/PyAutoGUI tests. Keeping WAD
opt-in lets the always-available UI layers run unconditionally while the
shell-dialog layer engages only when the service is present and current.
