# PDFE overlay port for mujs — hermetic override of the codeberg hash drift.
#
# WHY THIS EXISTS
#   Upstream vcpkg pins mujs 1.3.8 by SHA512 b553c09f… of the codeberg
#   source tarball (https://codeberg.org/ccxvii/mujs/archive/1.3.8.tar.gz).
#   Codeberg regenerates that archive, so the bytes it now serves hash to
#   528cd8a8… — vcpkg then aborts with "unexpected hash" and mujs fails to
#   build. The Windows self-hosted lane is immune only because its vcpkg
#   BINARY cache holds mujs pre-built (it never re-downloads the source).
#   Ephemeral runners (GitHub-hosted macOS, Hetzner Linux) have cold binary
#   caches for their triplets, rebuild mujs, hit the drifted download, and
#   fail. See docs/audit/ci and the task register.
#
# WHAT CHANGED vs the upstream port
#   ONLY the SHA512 — updated to the bytes codeberg currently serves. The
#   source TREE is byte-identical to the golden b553c09f… tarball (verified
#   by `diff -r` of both extracted trees + identical mujs.h SHA512); codeberg
#   only rewrapped the tar/compression, so this builds the exact same mujs
#   1.3.8 code. CMakeLists.txt / mujs.pc / vcpkg.json are copied verbatim.
#
#   VERIFICATION ARTIFACT (SEC-88 / GitHub #67)
#     docs/security/vcpkg-overlay-source-verification.md records the
#     re-derived check as a committed, dated artifact instead of only
#     this comment. Regenerate with tools/gate/verify_vcpkg_overlays.py.
# SCOPE
#   Applied via VCPKG_OVERLAY_PORTS on the ephemeral lanes only. The Windows
#   lane does not set the overlay and is unaffected (still served from its
#   binary cache). Remove this overlay once upstream vcpkg repins mujs.

if(VCPKG_TARGET_IS_WINDOWS)
  vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
endif()

vcpkg_from_github(
  OUT_SOURCE_PATH SOURCE_PATH
  GITHUB_HOST https://codeberg.org
  REPO ccxvii/mujs
  REF "${VERSION}"
  SHA512 528cd8a8a60fdef64fe5624739b773277990ea6c531f15b0219043076b0c8242d63ccb6fe2b0dd77420b9d74fe16b3f16259489667db37436d21ed8c745c15a6
  HEAD_REF master
)

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
