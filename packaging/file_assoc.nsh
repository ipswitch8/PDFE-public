; PDFE NSIS installer — file-association registry block.
;
; This file is !included from the CPack-generated project.nsi via
; CPACK_NSIS_EXTRA_INSTALL_COMMANDS. It lives in the repo (not the
; build dir) so its contents go into the .nsi VERBATIM with no
; CMake/CPack escape processing — the previous inline-string
; approach lost backslashes and quotes during CPackConfig round-trip
; and produced ``$"path$" $"%1$"`` in the registry, breaking the
; shell open command.

CreateShortCut "$DESKTOP\PDFE.lnk" "$INSTDIR\bin\pdfe.exe" "" "$INSTDIR\bin\pdfe.ico"

; --- ProgID: PDFE.Document ---
WriteRegStr HKCR "PDFE.Document" "" "PDF Document"
WriteRegStr HKCR "PDFE.Document" "FriendlyTypeName" "PDF Document"
WriteRegStr HKCR "PDFE.Document\DefaultIcon" "" "$INSTDIR\bin\pdfe.ico"
WriteRegStr HKCR "PDFE.Document\shell\open\command" "" '"$INSTDIR\bin\pdfe.exe" "%1"'
WriteRegStr HKCR "PDFE.Document\Application" "ApplicationCompany" "PDFE Project"
WriteRegStr HKCR "PDFE.Document\Application" "ApplicationDescription" "Professional PDF editor"
WriteRegStr HKCR "PDFE.Document\Application" "ApplicationName" "PDFE"

; --- Application registration: HKCR\Applications\pdfe.exe ---
; Drives the friendly name + icon Windows shows in the "Open with"
; picker. Without these, Windows displays the raw command path
; because it has no other label to use.
WriteRegStr HKCR "Applications\pdfe.exe" "FriendlyAppName" "PDFE - Professional PDF Editor"
WriteRegStr HKCR "Applications\pdfe.exe\DefaultIcon" "" "$INSTDIR\bin\pdfe.exe,0"
WriteRegStr HKCR "Applications\pdfe.exe\shell\open\command" "" '"$INSTDIR\bin\pdfe.exe" "%1"'
WriteRegStr HKCR "Applications\pdfe.exe\SupportedTypes" ".pdf" ""

; --- File-extension hooks for .pdf ---
WriteRegStr HKCR ".pdf\OpenWithProgids" "PDFE.Document" ""
WriteRegStr HKLM "Software\Classes\.pdf\OpenWithProgids" "PDFE.Document" ""

; --- App Paths so 'pdfe' on Run/cmd resolves ---
WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\pdfe.exe" "" "$INSTDIR\bin\pdfe.exe"
WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\pdfe.exe" "Path" "$INSTDIR\bin"

; --- RegisteredApplications + Capabilities ---
; Surfaces PDFE in Windows Settings -> Apps -> Default apps so the
; user can pick PDFE as the default for .pdf without the installer
; trying (and failing) to forge a UserChoice.
WriteRegStr HKLM "Software\PDFE\Capabilities" "ApplicationName" "PDFE"
WriteRegStr HKLM "Software\PDFE\Capabilities" "ApplicationIcon" "$INSTDIR\bin\pdfe.exe,0"
WriteRegStr HKLM "Software\PDFE\Capabilities" "ApplicationDescription" "Professional PDF editor."
WriteRegStr HKLM "Software\PDFE\Capabilities\FileAssociations" ".pdf" "PDFE.Document"
WriteRegStr HKLM "Software\RegisteredApplications" "PDFE" "Software\PDFE\Capabilities"

; Notify shell so cached associations refresh without reboot.
System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0x0000, p 0, p 0)'
