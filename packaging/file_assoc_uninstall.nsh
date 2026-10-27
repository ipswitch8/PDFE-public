; PDFE NSIS uninstaller — undo the registry block from file_assoc.nsh.

Delete "$DESKTOP\PDFE.lnk"
DeleteRegKey HKCR "PDFE.Document"
DeleteRegValue HKCR ".pdf\OpenWithProgids" "PDFE.Document"
DeleteRegValue HKLM "Software\Classes\.pdf\OpenWithProgids" "PDFE.Document"
DeleteRegKey HKCR "Applications\pdfe.exe"
DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\pdfe.exe"
DeleteRegKey HKLM "Software\PDFE"
DeleteRegValue HKLM "Software\RegisteredApplications" "PDFE"
System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0x0000, p 0, p 0)'
