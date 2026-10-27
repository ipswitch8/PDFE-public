# GitHub #239 (sibling of #214) — repo-local overlay for gumbo 0.12.3.
#
# Same defect as mujs: Codeberg runs Gitea, Gitea-generated archive tarballs are
# not byte-reproducible, and the tag gets re-packed with different framing. The
# SOURCE does not change; the whole-file hash does.
#
# THREE hashes now exist for this one tag:
#
#   15da29bc…  the pinned baseline this repo resolved
#   612b570b…  upstream vcpkg's refresh, 2026-08-02, whose own comment says
#              "refresh the hash when download fails with hash mismatch"
#   4e0bc0e8…  what Codeberg serves today  <- pinned below
#
# Upstream's remedy is a treadmill and was already stale by the time we hit it.
# Pinning is therefore a holding action, not a fix; the durable answer is an
# asset cache that takes Codeberg off the critical path for every port at once,
# which is #177's ground. #239 carries the costed options.
#
# WHAT THE HASH BELOW IS ANCHORED TO, AND HOW STRONGLY.
#
# Unlike mujs, gumbo has no clean extraction on this machine to compare against
# in full. The first attempt compared against libmupdf's vendored
# thirdparty/gumbo-parser directory and found ZERO common files -- it refused,
# because "no differences among zero files" is a vacuous pass, not a check.
#
# The anchor that does exist is narrow and is stated as such: the header
# build/repro-full/installed/x64-windows-repro/include/gumbo.h was installed by
# vcpkg from the ORIGINAL Codeberg tarball while its SHA512 still matched the
# pin. The gumbo.h inside today's archive is byte-identical to it
# (sha256 36df027de61351abf7f2e9b9445bc59a452ad13800bbb60046fd91f85e567f30).
#
# That is ONE public header. It says the API surface is unchanged. It does NOT
# prove the implementation is unchanged -- for mujs all 53 source files were
# compared, and this is not that. It is corroboration, not proof, and it is
# better than upstream's refresh, which carries no anchor at all.
#
# Re-check with tests/_runtime/i236/verify_gumbo2.py.

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_download_distfile(ARCHIVE
    URLS "https://codeberg.org/gumbo-parser/gumbo-parser/archive/${VERSION}.tar.gz"
    FILENAME "gumbo-${VERSION}.tar.gz"
    SHA512 4e0bc0e8c387bad18cde5f9998930afedbe4e38d2e984dd14c87486f4b86e5eda6089b8c11ceda0827be0a18a075bfeeafedb068694f30fe3f2b3a8be0238b07
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
