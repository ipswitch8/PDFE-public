# Installer Residue & HKCU User-Data Intent

This note documents PDFE's install/uninstall symmetry and the deliberate
handling of per-user (HKCU) data on uninstall. It is the authoritative
reference for the installer-residue regression tests in
`tests/test_installer.cpp` (the `InstallerResidue_*` suite), which parse the
authored installer sources and assert against the intent recorded here.

## Installer sources

| Artifact | Created by | Removed by |
|---|---|---|
| Installed files under `$INSTDIR\bin` (exe, DLLs, tessdata, model, Qt runtime) | `install(...)` rules + `windeployqt` (root `CMakeLists.txt`) | CPack/NSIS auto-generated uninstaller (`Delete`/`RMDir` from the install manifest) |
| Start-menu shortcut `$SMPROGRAMS\$STARTMENU_FOLDER\PDFE.lnk` | `CPACK_NSIS_CREATE_ICONS_EXTRA` | `CPACK_NSIS_DELETE_ICONS_EXTRA` |
| Desktop shortcut `$DESKTOP\PDFE.lnk` | `packaging/file_assoc.nsh` | `packaging/file_assoc_uninstall.nsh` |
| File-association / ProgID / capabilities registry keys (HKCR + HKLM) | `packaging/file_assoc.nsh` | `packaging/file_assoc_uninstall.nsh` |
| Uninstall registry key (`HKLM\...\Uninstall\PDFE`, DisplayName etc.) | CPack/NSIS auto | CPack/NSIS auto (uninstaller self-deletes it) |

Every install-time file, shortcut, and registry write listed above has a
matching uninstall directive. The `windeployqt` step writes inside
`${CMAKE_INSTALL_PREFIX}/bin`, so its output is captured in the CPack install
manifest and removed with the rest of `$INSTDIR` — no Qt-runtime residue.

## HKCU user-data intent: **PRESERVED (deliberately not removed)**

At runtime PDFE persists user data through `QSettings` in its default
(`NativeFormat`) backend. With `QApplication` organization name `PDFE` and
application name `PDFE` (`src/main.cpp`), Windows maps this to:

```
HKCU\Software\PDFE\PDFE
```

This subtree holds recent-files lists, UI/tool preferences
(`src/app/AppSettings.cpp`), the image-stamp library
(`src/annotations/ImageStampLibrary.cpp`), and signature trust anchors
(`src/signatures/TrustAnchorManager.cpp`).

**Decision: the uninstaller deliberately does NOT delete
`HKCU\Software\PDFE`.** User data is preserved across uninstall.

Rationale:

1. **Convention.** This matches standard Windows / Qt desktop-app behavior:
   preferences survive an uninstall so an upgrade or reinstall keeps the
   user's settings, trust anchors, and stamp library.
2. **Per-user hive scoping.** An elevated uninstaller runs against a single
   HKCU hive and cannot correctly enumerate or clear the settings of every
   Windows user profile on the machine; a partial wipe would be worse than a
   clean preserve.
3. **User control.** A user who wants a full wipe can clear it explicitly
   (delete `HKCU\Software\PDFE` via `regedit`, or a future "remove settings"
   option). Preservation is reversible; deletion at uninstall is not.

**Scope guard.** The machine-wide app registration the installer writes at
`HKLM\Software\PDFE\Capabilities` is a *different hive* from the user-data
`HKCU\Software\PDFE`. The uninstaller removes only the HKLM key
(`DeleteRegKey HKLM "Software\PDFE"`); it must never issue
`DeleteRegKey HKCU "Software\PDFE"`. The regression test enforces both halves
of this intent: the HKLM key is removed, and no HKCU deletion is present.

## Install scope: **per-machine, administrator-only (deliberate)**

The installer is **per-machine only and requires administrator rights.** It
writes machine-wide keys (`HKLM\Software\PDFE\Capabilities`,
`HKLM\Software\RegisteredApplications`, `App Paths\pdfe.exe`, the
`HKLM\...\Uninstall\PDFE` ARP key) and installs into a machine-wide prefix, none
of which a standard user can write.

**Decision: there is deliberately NO per-user (HKCU-scoped) install mode.**
Rationale: the entire value of the registration block is shell integration —
appearing in "Open with", being selectable in Settings → Default apps, and
resolving `pdfe` from Run/cmd. A per-user variant would have to duplicate every
one of those writes under `HKCU\Software\Classes` and maintain two divergent
uninstall paths, for a scenario (a standard user on a managed machine) where
policy usually blocks unmanaged software regardless. The consequence is
accepted and stated: **a non-administrator cannot install PDFE**, and the
requirement is stated on the download page in `README.md` rather than being
discovered at install time.

## Repair mode: **not available by construction (deliberate)**

PDFE ships an NSIS installer, not an MSI. NSIS has no installer database and
therefore no Windows Installer "Repair" verb: Apps & Features offers *Uninstall*
only, and there is no `/repair` switch to add. **Decision: repair mode is out of
scope; the supported recovery path is uninstall followed by reinstall**, which
is lossless for user data precisely because of the HKCU preservation intent
recorded above — settings, trust anchors and the stamp library survive the
round trip. Migrating to WiX/MSI purely to gain a Repair verb is not justified
at this release cadence; if it is ever adopted for other reasons (per-user
install, group-policy deployment) repair comes with it for free.
