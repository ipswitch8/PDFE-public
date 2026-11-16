// GitHub #390 — POSIX remote-filesystem detection (path_is_on_network_filesystem)
// and its wiring into the is_unc_or_remote gate.
//
// On Windows a mapped drive is remote although nothing in "Z:\x.pdf" says so;
// drive_letter_resolves_to_remote asks the drive table. The POSIX analogue is
// a mount: "/srv/data/x.pdf" is remote when /srv/data is NFS. These cases pin the
// classification (statfs magic, FUSE subtype, mountinfo matching), the local
// and error answers on this host's real mounts, and, through the PDFE_TESTING
// probe seam, the remote answer and its effect on Document::open — without
// needing a network mount on the test machine.
//
// POSIX-only by construction: on Windows the function is the constant false
// and the drive-table probe is tested in test_path_security.cpp.

#include <gtest/gtest.h>

#if !defined(_WIN32)

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PathSecurity.h"

#include <cstring>
#include <memory>
#include <string>

namespace ps = pdfe::core::path_security;

namespace {

constexpr unsigned long long kNfs = 0x6969ULL;
constexpr unsigned long long kExt4 = 0xEF53ULL;
constexpr unsigned long long kFuse = 0x65735546ULL;

// Seam fakes. A fake answers for EXACT paths only, so a deeper path that does
// not "exist" must be classified by climbing to its nearest ancestor.
bool fakeNfsShare(const char* path, unsigned long long* magic)
{
    if (std::strcmp(path, "/nfs-share") == 0) { *magic = kNfs; return true; }
    if (std::strcmp(path, "/") == 0) { *magic = kExt4; return true; }
    return false;
}

bool fakeFuseRoot(const char* path, unsigned long long* magic)
{
    if (std::strcmp(path, "/") == 0) { *magic = kFuse; return true; }
    return false;
}

// A FUSE mount at /mnt/sshbox (statfs magic), whose mountinfo entry says sshfs.
bool fakeSshfsMount(const char* path, unsigned long long* magic)
{
    if (std::strcmp(path, "/mnt/sshbox") == 0) { *magic = kFuse; return true; }
    if (std::strcmp(path, "/") == 0) { *magic = kExt4; return true; }
    return false;
}

std::string fakeMountinfo()
{
    return "21 1 0:20 / / rw - ext4 /dev/root rw\n"
           "40 21 0:40 / /mnt/sshbox rw - fuse.sshfs u@host: rw\n"
           "41 21 0:41 / /mnt/ntfs rw - fuseblk /dev/sdd1 rw\n";
}

bool fakeNtfsMount(const char* path, unsigned long long* magic)
{
    if (std::strcmp(path, "/mnt/ntfs") == 0) { *magic = kFuse; return true; }
    if (std::strcmp(path, "/") == 0) { *magic = kExt4; return true; }
    return false;
}

class NetworkFsSeam : public ::testing::Test {
protected:
    void TearDown() override
    {
        ps::testseam::setFsMagicProbe(nullptr);
        ps::testseam::setMountinfoReader(nullptr);
    }
};

}  // namespace

TEST(NetworkFilesystemDetection, NetworkMagicNumbersAreRemote)
{
    for (unsigned long long m : {0x6969ULL, 0x517BULL, 0xFF534D42ULL, 0xFE534D42ULL,
                                 0x5346414FULL, 0x6B414653ULL, 0x73757245ULL, 0x564CULL,
                                 0x01021997ULL, 0x00C36400ULL, 0x0BD00BD0ULL,
                                 0x47504653ULL, 0xAAD7AAEAULL}) {
        EXPECT_TRUE(ps::detail::linux_fs_magic_is_network(m)) << std::hex << m;
    }
}

TEST(NetworkFilesystemDetection, LocalMagicNumbersAreNotRemote)
{
    // ext4, tmpfs, btrfs, xfs, zfs, overlayfs, procfs, squashfs, vfat — and
    // FUSE, whose one magic covers local and network subtypes alike.
    for (unsigned long long m : {0xEF53ULL, 0x01021994ULL, 0x9123683EULL, 0x58465342ULL,
                                 0x2FC12FC1ULL, 0x794C7630ULL, 0x9FA0ULL, 0x73717368ULL,
                                 0x4D44ULL, kFuse}) {
        EXPECT_FALSE(ps::detail::linux_fs_magic_is_network(m)) << std::hex << m;
    }
}

TEST(NetworkFilesystemDetection, FuseSubtypesAreClassifiedByName)
{
    for (const char* t : {"fuse.sshfs", "fuse.rclone", "fuse.s3fs", "fuse.glusterfs",
                          "fuse.gvfsd-fuse", "fuse.davfs2", "fuse.smbnetfs"}) {
        EXPECT_TRUE(ps::detail::fuse_fstype_is_network(t)) << t;
    }
    for (const char* t : {"fuseblk", "fuse", "fuse.portal", "fuse.ntfs-3g",
                          "fuse.cpsm.AppImage", "fusectl", "nfs", ""}) {
        EXPECT_FALSE(ps::detail::fuse_fstype_is_network(t)) << t;
    }
}

TEST(NetworkFilesystemDetection, MountinfoPicksTheLongestContainingMount)
{
    const std::string mi =
        "21 1 0:20 / / rw,relatime shared:1 - zfs rpool/ROOT rw\n"
        "30 21 0:30 / /srv/data rw,relatime shared:2 - nfs4 srv:/data rw,vers=4.2\n"
        "31 30 0:31 / /srv/data/local rw shared:3 - ext4 /dev/sdb1 rw\n"
        "32 21 0:32 / /mnt/my\\040share rw - cifs //srv/share rw\n"
        "33 21 0:33 / /mnt/sshbox rw - fuse.sshfs u@host: rw,user_id=1000\n"
        "34 21 0:34 / /tmp rw - tmpfs tmpfs rw\n"
        "35 34 0:35 / /tmp rw - ext4 /dev/sdc1 rw\n";
    using ps::detail::mountinfo_fstype_for;
    EXPECT_EQ(mountinfo_fstype_for(mi, "/etc/passwd"), "zfs");
    EXPECT_EQ(mountinfo_fstype_for(mi, "/srv/data/doc.pdf"), "nfs4");
    EXPECT_EQ(mountinfo_fstype_for(mi, "/srv/data/local/doc.pdf"), "ext4")
        << "a nested local mount inside an NFS mount must win";
    EXPECT_EQ(mountinfo_fstype_for(mi, "/srv/data"), "nfs4") << "the mount point itself";
    EXPECT_EQ(mountinfo_fstype_for(mi, "/srv/database/x"), "zfs")
        << "/srv/data must not match /srv/database: component boundary";
    EXPECT_EQ(mountinfo_fstype_for(mi, "/mnt/my share/a.pdf"), "cifs")
        << "\\040 in a mount point is a space";
    EXPECT_EQ(mountinfo_fstype_for(mi, "/mnt/sshbox/a.pdf"), "fuse.sshfs");
    EXPECT_EQ(mountinfo_fstype_for(mi, "/tmp/x"), "ext4")
        << "a later mount over the same point hides the earlier one";
    EXPECT_EQ(mountinfo_fstype_for("", "/x"), "");
    EXPECT_EQ(mountinfo_fstype_for("garbage line\n", "/x"), "");
}

TEST(NetworkFilesystemDetection, KernelFilesystemsOnThisHostAreLocal)
{
    // procfs and devtmpfs are local on every Linux system: a real statfs
    // through the real code path, no seam.
    EXPECT_FALSE(ps::path_is_on_network_filesystem("/proc"));
    EXPECT_FALSE(ps::path_is_on_network_filesystem("/dev"));
    EXPECT_FALSE(ps::is_unc_or_remote(std::string_view{"/proc/self"}));
}

TEST(NetworkFilesystemDetection, ErrorCasesAnswerNotRemote)
{
    EXPECT_FALSE(ps::path_is_on_network_filesystem(""));
    EXPECT_FALSE(ps::path_is_on_network_filesystem(std::string_view("/pro\0c", 6)))
        << "an embedded NUL names no path";
    // Does not exist: classified by its nearest existing ancestor (/proc).
    EXPECT_FALSE(ps::path_is_on_network_filesystem("/proc/no/such/dir/file.pdf"));
    // Relative paths are resolved against the working directory.
    EXPECT_EQ(ps::path_is_on_network_filesystem("no-such-relative-file.pdf"),
              ps::path_is_on_network_filesystem("."));
}

TEST_F(NetworkFsSeam, AnNfsMountIsRemoteAndSoIsAPathThatDoesNotExistYetUnderIt)
{
    ps::testseam::setFsMagicProbe(&fakeNfsShare);
    EXPECT_TRUE(ps::path_is_on_network_filesystem("/nfs-share"));
    EXPECT_TRUE(ps::path_is_on_network_filesystem("/nfs-share/new/sub/out.pdf"))
        << "a save target that does not exist yet must be classified by the "
           "mount it would be created on";
    EXPECT_FALSE(ps::path_is_on_network_filesystem("/local/doc.pdf"))
        << "climbing from a missing local path reaches / (ext4)";
}

TEST_F(NetworkFsSeam, FuseMagicAloneIsNotRemote)
{
    // FUSE is one magic for sshfs and for ntfs-3g. The verdict must come from
    // the mount's subtype in /proc/self/mountinfo; this host's "/" is not a
    // network FUSE mount, so a FUSE magic reported for "/" is still local.
    ps::testseam::setFsMagicProbe(&fakeFuseRoot);
    EXPECT_FALSE(ps::path_is_on_network_filesystem("/"));
}

TEST_F(NetworkFsSeam, ANetworkFuseMountIsRemoteEndToEnd)
{
    // The composition FuseMagicAloneIsNotRemote cannot show: a FUSE magic,
    // then the mount's subtype from mountinfo, decides "remote" for sshfs and
    // "local" for fuseblk (ntfs-3g), through the public entry point.
    ps::testseam::setMountinfoReader(&fakeMountinfo);

    ps::testseam::setFsMagicProbe(&fakeSshfsMount);
    EXPECT_TRUE(ps::path_is_on_network_filesystem("/mnt/sshbox/notes.pdf"));
    EXPECT_TRUE(ps::is_unc_or_remote(std::string_view{"/mnt/sshbox/notes.pdf"}));

    ps::testseam::setFsMagicProbe(&fakeNtfsMount);
    EXPECT_FALSE(ps::path_is_on_network_filesystem("/mnt/ntfs/notes.pdf"))
        << "a local FUSE filesystem (ntfs-3g) is not remote";
}

TEST_F(NetworkFsSeam, TheRemoteGateAsksTheFilesystemOnPosix)
{
    ps::testseam::setFsMagicProbe(&fakeNfsShare);
    EXPECT_TRUE(ps::is_unc_or_remote(std::string_view{"/nfs-share/report.pdf"}));
    EXPECT_TRUE(ps::is_unc_or_remote(std::wstring_view{L"/nfs-share/r\u00e9sum\u00e9.pdf"}))
        << "the wide overload must ask about the real path, not a lossy "
           "narrowing that names nothing";
    EXPECT_FALSE(ps::is_unc_or_remote(std::string_view{"/local/report.pdf"}));
    EXPECT_FALSE(ps::is_lexically_unc_or_remote(std::string_view{"/nfs-share/report.pdf"}))
        << "the lexical check alone must stay lexical";
}

TEST_F(NetworkFsSeam, DocumentContentCannotOpenAFileOnAnNfsMount)
{
    // The policy the gate feeds (GitHub #240): a PERSON may open a network
    // file; a document may not make this process reach one.
    ps::testseam::setFsMagicProbe(&fakeNfsShare);
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    using Origin = pdfe::core::Document::OpenOrigin;
    try {
        pdfe::core::Document::open(ctx, "/nfs-share/linked.pdf", "", Origin::DocumentRequested);
        FAIL() << "a document-requested open of an NFS path was not refused";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("network"), std::string::npos) << e.what();
    }
    try {
        pdfe::core::Document::open(ctx, "/nfs-share/linked.pdf", "", Origin::UserInitiated);
        FAIL() << "the fake path cannot exist, so the user-initiated open must fail";
    } catch (const std::exception& e) {
        EXPECT_EQ(std::string(e.what()).find("Refusing UNC / network"), std::string::npos)
            << "a user-initiated open was refused by the network policy: " << e.what();
    }
}

#endif  // !_WIN32
