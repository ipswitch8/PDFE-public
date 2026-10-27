# x64-windows-repro -- x64-windows, but built deterministically.
#
# WHY THIS EXISTS (GitHub #177)
#
# #177 asks for vcpkg binary-cache integrity "asserted via a checksum/signature
# verification step". You cannot pin a checksum to an artifact whose bytes change
# every time you build it, so the prerequisite is deterministic output.
#
# MEASURED on this host, 2026-09-14. Building the SAME source to the SAME output
# path twice, one second apart, with identical flags, produced DIFFERENT bytes:
#
#     pass1.dll  SHA-256 10B54C3E...1E15
#     pass2.dll  SHA-256 737A9CD0...B382     19 of 666,624 bytes differed
#
# Attributed exactly:
#   * 16 bytes -- the CodeView RSDS GUID. link.exe mints a fresh random GUID on
#     EVERY link; it is the key pairing an image with its PDB.
#   *  3 bytes -- the COFF TimeDateStamp (wall-clock seconds at link time),
#     appearing in the PE header and two debug-directory entries.
#
# `/Brepro` replaces both with content-derived values. Re-measured with it, the
# two builds were byte-IDENTICAL -- verified in three variants: with no debug
# info, with a PDB, and with a PDB plus a fixed PDB name. So determinism here is
# a linker flag, not a research project.
#
# A THIRD SOURCE OF VARIANCE THAT /Brepro DOES NOT FIX
#
# The image also embeds the ABSOLUTE path of its PDB. Measured on a real cached
# package in this tree:
#
#     archive.dll -> C:\Users\<account>\source\vcpkg\buildtrees\libarchive\
#                    x64-windows-rel\bin\archive.pdb
#
# So identical sources built under different vcpkg roots still differ, and the
# path carries the build account name (the same mechanism behind this project's
# account-name leak incidents -- here the consequence is hash instability rather
# than disclosure). `/PDBALTPATH:%_PDB%` writes just the basename instead, which
# removes both the variance and the embedded account name.
#
# WHAT IS STILL UNKNOWN, AND IS ANSWERED BY BUILDING RATHER THAN BY ARGUING
#
#   * Whether every port threads VCPKG_LINKER_FLAGS through. Ports driven by
#     meson / autotools / custom scripts may ignore it.
#   * Static `.lib` archives: COFF archive members carry their own timestamps,
#     which `/Brepro` (a link.exe flag) does not govern. x64-windows is dynamic
#     linkage, so this should be a minority, but "should be" is not a measurement.
#   * Ports that ship prebuilt binaries rather than building at all.
#
# Do not promote this triplet to the default until a two-build comparison has
# reported how many packages actually land byte-identical.
#
# SET VCPKG_HOST_TRIPLET TO THIS TRIPLET TOO, WHEREVER IT IS SELECTED.
# The host triplet defaults to x64-windows independently of the target triplet,
# and this project has a host dependency (gtest). Setting only the target
# triplet makes vcpkg resolve BOTH chains and install two parallel trees --
# observed directly: "brotli:x64-windows-repro" and "brotli:x64-windows" both
# listed for installation in the same configure. The build still succeeds, so
# nothing complains; you simply pay for a second dependency set that no check
# covers. CMakePresets.json sets both.

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# /Brepro          -- deterministic timestamp + CodeView GUID (measured above).
# /PDBALTPATH      -- record only the PDB basename in the image, so the absolute
#                     build path (and the account name in it) is not embedded.
#
# THE PER-CONFIG FLAGS ARE SET EXPLICITLY, AND THAT IS NOT BELT-AND-BRACES.
# With only the generic VCPKG_LINKER_FLAGS set, a two-pass build measured:
#
#   bin/sqlite3.dll          0x315B7485  0x315B7485   stable (content hash)
#   debug/bin/sqlite3.dll    0x6AA8DC74  0x6AA8DCCD   WALL CLOCK -- 89 s apart,
#                                                     exactly the gap between passes
#   debug/bin/brotlidec.dll  0x63319608  0x63319608   stable
#
# So the generic flag reaches every RELEASE link, but only SOME debug links:
# brotli honoured it, sqlite3 and pugixml did not, because their build systems
# replace rather than append the debug linker flags. Port-specific behaviour, not
# a vcpkg-wide rule -- which is why this has to be set per configuration and then
# re-measured rather than reasoned about.
#
# INCREMENTAL LINKING DEFEATS /Brepro, AND THAT IS THE ACTUAL CAUSE.
# Setting VCPKG_LINKER_FLAGS_DEBUG alone did NOT fix the two debug DLLs -- they
# still carried wall-clock stamps 72 s apart. Debug configurations link with
# /INCREMENTAL by default, and an incrementally linked image cannot carry a
# content-derived stamp, so the linker silently keeps the wall-clock one. brotli
# came out stable only because its build already disables incremental linking.
# Hence /INCREMENTAL:NO on the debug config specifically.
set(VCPKG_LINKER_FLAGS "/Brepro /PDBALTPATH:%_PDB%")

# A FOURTH SOURCE OF VARIANCE: __FILE__ (2026-10-03).
#
# /PDBALTPATH fixes the PDB path recorded in the image. It does NOT touch
# __FILE__, which assertion macros compile in as a string literal, absolute.
# Measured in the tree the runner built on 2026-10-03:
#
#   tesseract55.dll        106 strings  C:\actions-runner\_work\PDFE\PDFE\vcpkg\
#                                       buildtrees\tesseract\src\5.5.2-…\src\api\…
#   qschannelbackendd.dll  144 strings  C:/actions-runner/_work/…/buildtrees/qtbase/…
#   gmock_main.dll          10 strings  …\buildtrees\gtest\src\v1.17.0-…\…
#
# So identical sources built under different vcpkg roots still differ, which made
# docs/security/vcpkg-cache-manifest.json regenerable only on the CI machine and
# meant a workstation regeneration would pin the operator's account name. The
# comment above claiming /PDBALTPATH "removes both the variance and the embedded
# account name" was therefore incomplete.
#
# CONTROL FIRST, because the premise had to be falsifiable. gtest built twice
# from source, same toolchain, ONLY --x-buildtrees-root differing:
#
#   pass A  474c43bd…fe19   521728 bytes
#   pass B  df8c8313…4aee   521728 bytes      <-- same size, different bytes
#
# /d1trimfile:<prefix> strips <prefix> from __FILE__. CURRENT_BUILDTREES_DIR is
# per-port and is set by ports.cmake BEFORE the triplet is included, so trimming
# it leaves `src/<version>-<hash>.clean/...` -- still useful in a stack trace and
# identical on every machine, because that hash is content-derived.
#
# BOTH SEPARATOR FORMS ARE PASSED, and that is not belt-and-braces: the measured
# strings show tesseract embedding BACKslashes and qtbase embedding FORWARD
# slashes, so a single form would trim only some ports.
#
# /d1 is an undocumented MSVC family. It is set here rather than globally for
# that reason: the deterministic triplet is already opt-in and measured, and
# `vcpkg_cache_manifest.py --verify` refuses a tree built under any other
# triplet, so the blast radius is this triplet's own output.
#
# NO TRAILING SEPARATOR ON THE NATIVE FORM, and that is a measured correction
# rather than a style choice. A first version ended the backslash variant with a
# trailing `\`, which escaped the cache delimiter and ran the value into the next
# entry -- observed directly in a port's CMakeCache.txt:
#
#   CMAKE_CXX_FLAGS:STRING= ... /d1trimfile:C:\...\detect_compiler;-DVCPKG_CXX_FLAGS_RELEASE=
#                                                                 ^ value bled into the next variable
#
# Trimming the directory WITHOUT its separator simply leaves the leading `\` or
# `/` on what remains, which is harmless, so the forward-slash form keeps its
# trailing `/` and the native form has none.
file(TO_NATIVE_PATH "${CURRENT_BUILDTREES_DIR}" _repro_bt_native)
set(_repro_trim "/d1trimfile:${CURRENT_BUILDTREES_DIR}/ /d1trimfile:${_repro_bt_native}")
set(VCPKG_CXX_FLAGS "${_repro_trim}")
set(VCPKG_C_FLAGS "${_repro_trim}")

# WHY THE DEBUG FIX IS A CONFIGURE OPTION AND NOT A LINKER FLAG.
# Setting VCPKG_LINKER_FLAGS_DEBUG does not work, and the build log says exactly
# why. vcpkg PREPENDS triplet linker flags to CMake's own per-config defaults,
# so the debug link line came out as:
#
#   /nologo /Brepro /PDBALTPATH:%_PDB% /INCREMENTAL:NO  /debug /INCREMENTAL
#                                      ^^^^^^^^^^^^^^^           ^^^^^^^^^^^
#                                      mine                      CMake's, LAST
#
# link.exe is last-wins, so CMake's trailing /INCREMENTAL re-enabled incremental
# linking, and an incrementally linked image cannot carry a content-derived
# timestamp -- so /Brepro was silently ignored. Release was unaffected because
# CMake's release default is already /INCREMENTAL:NO.
#
# Prepending can never win that argument, so the per-config default is REPLACED
# instead. The replacement keeps /debug (debug info is still wanted) and only
# flips the incremental setting.
# NOTE: because this REPLACES the variable rather than adding to it, the repro
# flags must be repeated here. A first attempt set only "/debug /INCREMENTAL:NO"
# and thereby discarded the /Brepro vcpkg had prepended -- which made the debug
# DLLs differ by 5,642 bytes instead of the 1 byte they differed by before the
# "fix". A change that makes the measurement worse is the fastest way to find out
# you removed something.
set(_repro_link "/Brepro /PDBALTPATH:%_PDB%")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_SHARED_LINKER_FLAGS_DEBUG=${_repro_link} /debug /INCREMENTAL:NO"
    "-DCMAKE_EXE_LINKER_FLAGS_DEBUG=${_repro_link} /debug /INCREMENTAL:NO"
    "-DCMAKE_MODULE_LINKER_FLAGS_DEBUG=${_repro_link} /debug /INCREMENTAL:NO")
