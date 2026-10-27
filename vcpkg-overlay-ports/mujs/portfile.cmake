# GitHub #214 — repo-local overlay for mujs 1.3.8, fetched from GitHub instead
# of Codeberg.
#
# WHY THIS EXISTS. The baseline port fetches
# https://codeberg.org/ccxvii/mujs/archive/1.3.8.tar.gz and pins its SHA512.
# Codeberg runs Gitea, and Gitea-generated archive tarballs are NOT
# byte-reproducible: the archive was regenerated with different gzip/tar
# framing, the whole-file hash changed, and every clean configure has failed at
# download since. The SOURCE did not change -- only the container did.
#
# That makes this a concrete instance of the unpinnable-upstream-artifact
# problem tracked in #177: a hash pin against an auto-generated archive is a pin
# against something the host may rebuild at any time.
#
# WHY 1.3.8 AND NOT 1.3.10. Upstream vcpkg has since moved this port to
# ArtifexSoftware/mujs on GitHub AND bumped it to 1.3.10. Adopting upstream
# verbatim would therefore change the JavaScript engine that executes PDF form
# JS. That is a real behavioural change needing its own form-JS regression pass,
# and it has no business riding along inside a fix whose only job is to make the
# project buildable again. The engine stays frozen at 1.3.8; the bump is a
# separate decision.
#
# WHY THE HASH BELOW IS NOT SELF-ATTESTED. Recomputing a hash for whatever you
# just downloaded defeats the point of pinning one. This SHA512 was verified by
# CONTENT against bytes the project had already hash-verified:
# build/repro-full/buildtrees/mujs/src/1.3.8-cedaea2660.clean is an extraction
# vcpkg validated against the ORIGINAL Codeberg SHA512 before the regeneration.
# All 53 source files in this GitHub tarball are byte-identical (SHA-256) to
# that tree, with nothing missing and nothing extra -- the only two files in the
# reference absent from any upstream archive are CMakeLists.txt and mujs.pc,
# which this port supplies itself, exactly as the baseline port does.
#
# So the new hash attests the same content the old hash attested. Re-verify with
# tests/_runtime/i236/verify_mujs.py if the reference tree still exists.

if(VCPKG_TARGET_IS_WINDOWS)
  vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
endif()

vcpkg_from_github(
  OUT_SOURCE_PATH SOURCE_PATH
  REPO ArtifexSoftware/mujs
  REF "${VERSION}"
  SHA512 8156326b7cd2734242ce91ac2e80ae7a3cae30448c8e1a6601e6a3b747db34c713eec630a3f873d5abb1d19a3e8a84845964c8ff03de595ea9acd371278f2623
  HEAD_REF master
)

# Unchanged from the baseline port: mujs ships no CMake build, so the port
# supplies one plus a pkg-config file.
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/mujs.pc" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
    "-DPACKAGE_VERSION=${VERSION}"
  OPTIONS_DEBUG
    -DDISABLE_INSTALL_HEADERS=ON
)

vcpkg_cmake_install()
vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-mujs)
vcpkg_copy_pdbs()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
