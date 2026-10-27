# PDFE overlay port for gumbo — hermetic override of the codeberg hash drift.
#
# Same root cause as tools/vcpkg-overlays/mujs: codeberg regenerated the
# gumbo-parser 0.12.3 source archive, so its bytes no longer hash to vcpkg's
# pinned SHA512 15da29bc… (they now hash to 4e0bc0e8…). Cold-binary-cache
# rebuilds on ephemeral runners (macOS-hosted, Hetzner Linux) then abort with
# "unexpected hash". The Windows lane is immune (gumbo served pre-built from its
# vcpkg binary cache).
#
# WHAT CHANGED vs the upstream port: ONLY the SHA512, repinned to the bytes
# codeberg currently serves at the exact URL vcpkg itself pins
# (https://codeberg.org/gumbo-parser/gumbo-parser/archive/0.12.3.tar.gz). The
# tarball was verified to be the authentic gumbo 0.12.3 upstream source
# (configure.ac AC_INIT([gumbo],[0.12.3]); src/gumbo.h + doc/COPYING present).
# CMakeLists.txt / vcpkg.json are copied verbatim. Applied via VCPKG_OVERLAY_PORTS
# on the ephemeral lanes only; remove once upstream vcpkg repins gumbo.
#
#   VERIFICATION ARTIFACT (SEC-88 / GitHub #67)
#     docs/security/vcpkg-overlay-source-verification.md records the
#     re-derived check as a committed, dated artifact instead of only
#     this comment. Regenerate with tools/gate/verify_vcpkg_overlays.py.

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_download_distfile(ARCHIVE
    URLS "https://codeberg.org/gumbo-parser/gumbo-parser/archive/${VERSION}.tar.gz"
    FILENAME "gumbo-${VERSION}.tar.gz"
    SHA512  4e0bc0e8c387bad18cde5f9998930afedbe4e38d2e984dd14c87486f4b86e5eda6089b8c11ceda0827be0a18a075bfeeafedb068694f30fe3f2b3a8be0238b07
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    SOURCE_BASE "${VERSION}"
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-gumbo CONFIG_PATH share/unofficial-gumbo)

vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/doc/COPYING")
