// -*- coding: utf-8 -*-
// =============================================================================
// WIN-87 wider class, phase-60 — the temp files that are not documents.
// =============================================================================
//
// The eight (then nine) WIN-87 publish paths all ended in a rename, which is
// what made a rename-shape sweep able to find them. This family does not:
//
//   * a WORKING file a by-name producer writes and something else re-reads BY
//     PATH later — clipboard image paste, the combine service's image wrappers;
//   * a FIXED-NAME %TEMP% write, which is a plant-and-redirect target rather
//     than a substitution target.
//
// These cases exercise the primitives that close both, and — because a
// primitive nothing calls proves nothing — they also pin that the two product
// sites named in the phase actually route through them.
//
// WHAT IS DELIBERATELY *NOT* CLAIMED. FILE_FLAG_OPEN_REPARSE_POINT protects the
// FINAL path component only; a junction planted on a PARENT directory is still
// traversed. That is stated in the reparse case below rather than left for
// someone to discover, because a guard whose limits are undocumented gets cited
// for coverage it does not have — the species this project logs as DCA 0023.
#include <gtest/gtest.h>

#include "core/TempFileSafety.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
// POSIX stand-ins for the MSVC CRT descriptor calls used below.
static inline int _write(int fd, const void* buf, unsigned n) { return static_cast<int>(::write(fd, buf, n)); }
static inline int _close(int fd) { return ::close(fd); }
#endif

namespace fs = std::filesystem;
using pdfe::core::PinnedTempFile;

namespace {

fs::path repoRoot() { return fs::path(PDFE_REPO_ROOT); }

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

bool writeFile(const fs::path& p, const std::string& bytes)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

fs::path tempDir()
{
    std::error_code ec;
    return fs::temp_directory_path(ec);
}

/// Strip // and /* */ comments so a source guard cannot be satisfied — or
/// falsely failed — by prose. This exact mistake produced two false failures in
/// phase-60's sibling workflow sweep, in a file whose `on:` block EXPLAINED in a
/// comment why it had no schedule.
std::string stripComments(const std::string& src)
{
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size();) {
        if (src.compare(i, 2, "//") == 0) {
            while (i < src.size() && src[i] != '\n') ++i;
        } else if (src.compare(i, 2, "/*") == 0) {
            i += 2;
            while (i + 1 < src.size() && src.compare(i, 2, "*/") != 0) ++i;
            i = (i + 1 < src.size()) ? i + 2 : src.size();
        } else {
            out.push_back(src[i]);
            ++i;
        }
    }
    return out;
}

}  // namespace

// -----------------------------------------------------------------------------
// The naming primitive — acceptance criterion 4.
// -----------------------------------------------------------------------------

TEST(TempFileSafetyPhase60, ReservedNamesAreUnlinkableAcrossManyReservations)
{
    // CombineFilesService's old name was
    //   "pdfe_combine_img_" + (uintptr_t)&p + "_" + index
    // which the reverification described as random. Two consecutive runs of
    // that scheme differ only in a counter from 0 and an allocator-stable
    // address, so a name from one run predicts the next. This asserts the
    // replacement does not have that property.
    constexpr int kSamples = 256;
    std::vector<std::unique_ptr<PinnedTempFile>> held;
    std::set<std::string> names;
    held.reserve(kSamples);

    for (int i = 0; i < kSamples; ++i) {
        std::error_code ec;
        auto f = PinnedTempFile::reserve(tempDir(), "pdfe_entropy_probe", ".bin", ec);
        ASSERT_TRUE(f) << "reserve failed at sample " << i << ": " << ec.message();
        names.insert(f->path().filename().string());
        held.push_back(std::move(f));
    }
    EXPECT_EQ(names.size(), static_cast<size_t>(kSamples))
        << "reserved names collided — the name is not 128 bits of CSPRNG";

    // Per-bit balance. A counter leaves the high nibbles constant across every
    // sample; a stack address leaves most of the word constant. Each of the 128
    // bits must vary, and each must be set somewhere between 25% and 75% of the
    // time — loose enough that a correct CSPRNG will not flake at this sample
    // size, tight enough that a counter or an address fails on the first
    // constant bit.
    std::array<int, 128> onesPerBit{};
    for (const auto& name : names) {
        // "pdfe_entropy_probe.<32 hex>.bin"
        const size_t dot = name.find('.');
        ASSERT_NE(dot, std::string::npos) << name;
        const std::string hex = name.substr(dot + 1, 32);
        ASSERT_EQ(hex.size(), 32u) << name;
        for (int nib = 0; nib < 32; ++nib) {
            const char c = hex[static_cast<size_t>(nib)];
            ASSERT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
                << "non-hex digit in reserved name: " << name;
            const int v = (c <= '9') ? (c - '0') : (c - 'a' + 10);
            for (int b = 0; b < 4; ++b) {
                if (v & (1 << b)) ++onesPerBit[static_cast<size_t>(nib * 4 + b)];
            }
        }
    }
    for (size_t bit = 0; bit < onesPerBit.size(); ++bit) {
        EXPECT_GT(onesPerBit[bit], kSamples / 4)
            << "bit " << bit << " is nearly always 0 across " << kSamples
            << " reservations — that is a counter or an address, not a CSPRNG";
        EXPECT_LT(onesPerBit[bit], kSamples - kSamples / 4)
            << "bit " << bit << " is nearly always 1 across " << kSamples
            << " reservations — that is a counter or an address, not a CSPRNG";
    }
}

TEST(TempFileSafetyPhase60, ReserveCreatesTheFileAndTheDestructorRemovesIt)
{
    fs::path path;
    {
        std::error_code ec;
        auto f = PinnedTempFile::reserve(tempDir(), "pdfe_lifecycle", ".bin", ec);
        ASSERT_TRUE(f) << ec.message();
        path = f->path();
        // CREATE_NEW: the file exists and is ours, so a pre-planted file at this
        // name would have failed the reserve rather than been adopted. It is
        // held from this instant, not merely from the first write.
        EXPECT_TRUE(fs::exists(path));
        EXPECT_EQ(fs::file_size(path), 0u);
        EXPECT_TRUE(f->isPinned());
    }
    EXPECT_FALSE(fs::exists(path))
        << "~PinnedTempFile must delete the file — the clipboard paste path used "
           "to leave every pasted image in %TEMP% forever";
}

// -----------------------------------------------------------------------------
// The substitution guarantee — the property acceptance criterion 3 is about.
// -----------------------------------------------------------------------------

#if defined(_WIN32)
TEST(TempFileSafetyPhase60, AHeldFileCannotBeOverwrittenINPLACEAtAnyPoint)
{
    // THE CASE THE PHASE-7 karen GATE FORCED, and the reason this class no
    // longer has a release-then-verify step at all.
    //
    // The first design reserved the name, CLOSED the handle so a by-name
    // producer could write it, then re-opened and compared the volume serial +
    // file index. That check sees delete-and-recreate. It does NOT see an
    // attacker who opens the SAME file object and overwrites it in place —
    // which is byte-for-byte the operation the legitimate producer performs, so
    // no identity check could ever have separated them. The window was real and
    // unwatchable.
    //
    // This asserts the replacement property: there is no such window, because
    // the handle is never released and no second writer can exist.
    std::error_code ec;
    auto f = PinnedTempFile::reserve(tempDir(), "pdfe_inplace", ".bin", ec);
    ASSERT_TRUE(f) << ec.message();
    ASSERT_TRUE(f->isPinned());

    const int fd = f->openWriteFd(ec);
    ASSERT_GE(fd, 0) << ec.message();
    const std::string genuine = "GENUINE-PRODUCER-BYTES";
    ASSERT_EQ(::_write(fd, genuine.data(), static_cast<unsigned>(genuine.size())),
              static_cast<int>(genuine.size()));
    ::_close(fd);

    // The attacker, at the one instant the old design was blind at: the file
    // exists, the producer has finished, the consumer has not read yet.
    HANDLE w = ::CreateFileW(f->path().wstring().c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_EQ(w, INVALID_HANDLE_VALUE)
        << "the held file was opened for writing — an in-place overwrite here is "
           "invisible to any file-identity check, which is why holding the handle "
           "replaced checking the identity";
    if (w != INVALID_HANDLE_VALUE) ::CloseHandle(w);

    EXPECT_EQ(readFile(f->path()), genuine);
}
#endif

TEST(TempFileSafetyPhase60, TheProducerCanWriteThroughTheHeldHandleAndAConsumerCanReadItBack)
{
    // The vacuity guard for everything above. A class that simply refused every
    // operation would satisfy the hostile cases and be useless: the producer
    // must be able to WRITE (through the handle) and a by-path consumer must be
    // able to READ. Both are asserted here, so "nothing can touch it" cannot
    // quietly become "nothing can use it".
    std::error_code ec;
    auto f = PinnedTempFile::reserve(tempDir(), "pdfe_usable", ".bin", ec);
    ASSERT_TRUE(f) << ec.message();

    const int fd = f->openWriteFd(ec);
    ASSERT_GE(fd, 0) << ec.message();
    const std::string payload = "PRODUCER-OUTPUT";
    ASSERT_EQ(::_write(fd, payload.data(), static_cast<unsigned>(payload.size())),
              static_cast<int>(payload.size()));
    ::_close(fd);

    // Closing the producer's descriptor must NOT release the object's hold.
    EXPECT_TRUE(f->isPinned());
    EXPECT_EQ(readFile(f->path()), payload)
        << "a by-path consumer could not read the held file — the pin locked out "
           "the very re-read it exists to protect";
}

#if defined(_WIN32)
TEST(TempFileSafetyPhase60, APinnedFileCannotBeWrittenDeletedOrRenamedOver)
{
    std::error_code ec;
    auto f = PinnedTempFile::reserve(tempDir(), "pdfe_pinned", ".bin", ec);
    ASSERT_TRUE(f) << ec.message();
    const int fd = f->openWriteFd(ec);
    ASSERT_GE(fd, 0) << ec.message();
    const std::string content = "PINNED-CONTENT";
    ASSERT_EQ(::_write(fd, content.data(), static_cast<unsigned>(content.size())),
              static_cast<int>(content.size()));
    ::_close(fd);
    const fs::path path = f->path();

    // 1. No writer. This is the attack that matters for clipboard paste: the
    //    path stays live for the whole session because redo() re-reads it.
    HANDLE w = ::CreateFileW(path.wstring().c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_EQ(w, INVALID_HANDLE_VALUE)
        << "a pinned file was opened for writing — an attacker can rewrite the "
           "image every redo() re-reads";
    if (w != INVALID_HANDLE_VALUE) ::CloseHandle(w);

    // 2. No deleter, and 3. no renamer — both fall out of withholding
    //    FILE_SHARE_DELETE, and a rename-over is the substitution primitive the
    //    whole WIN-87 class turns on.
    EXPECT_EQ(::DeleteFileW(path.wstring().c_str()), 0)
        << "a pinned file was deleted";

    const fs::path decoy = path.parent_path() / L"pdfe_pinned_decoy.bin";
    ASSERT_TRUE(writeFile(decoy, "ATTACKER-BYTES"));
    EXPECT_EQ(::MoveFileExW(decoy.wstring().c_str(), path.wstring().c_str(),
                            MOVEFILE_REPLACE_EXISTING), 0)
        << "an attacker renamed their file over a pinned one";
    fs::remove(decoy, ec);

    // 4. And the consumer can still read it by path, which is the whole point:
    //    the pin must not lock out the re-read it exists to protect.
    EXPECT_EQ(readFile(path), content);
}
#else  // POSIX
// -----------------------------------------------------------------------------
// POSIX (Linux port, phase 6). The Windows property — no second writer, deleter
// or renamer while held — CANNOT hold here: POSIX has no share modes. What is
// asserted instead is the documented best-effort substitute: after seal(), a
// by-path open for writing fails for this very user, and every way a
// same-user attacker can still change what the path names (rename-over,
// delete, chmod-back-then-write) is DETECTED by verifyUnchanged().
// -----------------------------------------------------------------------------
namespace {
std::unique_ptr<PinnedTempFile> producedAndSealed(const char* prefix,
                                                  const std::string& content)
{
    std::error_code ec;
    auto f = PinnedTempFile::reserve(tempDir(), prefix, ".bin", ec);
    if (!f) return nullptr;
    const int fd = f->openWriteFd(ec);
    if (fd < 0) return nullptr;
    if (::_write(fd, content.data(), static_cast<unsigned>(content.size()))
        != static_cast<int>(content.size())) {
        ::_close(fd);
        return nullptr;
    }
    ::_close(fd);
    if (!f->seal(ec)) return nullptr;
    return f;
}
}  // namespace

TEST(TempFileSafetyPhase60, PosixSealedFileCannotBeOpenedForWritingByPath)
{
    // Root bypasses permission bits (CAP_DAC_OVERRIDE); the guarantee is stated
    // for unprivileged processes, so the precondition is asserted, not assumed.
    ASSERT_NE(::geteuid(), 0u) << "this property is defined for non-root users";
    auto f = producedAndSealed("pdfe_seal_write", "GENUINE-PRODUCER-BYTES");
    ASSERT_TRUE(f);
    EXPECT_TRUE(f->isSealed());

    struct stat st {};
    ASSERT_EQ(::stat(f->path().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0400u) << "seal() did not drop the mode to 0400";

    errno = 0;
    const int w = ::open(f->path().c_str(), O_WRONLY | O_CLOEXEC);
    const int err = errno;
    EXPECT_LT(w, 0) << "a sealed file was opened for writing by path";
    if (w >= 0) ::close(w);
    EXPECT_EQ(err, EACCES);
    // O_TRUNC is the in-place overwrite primitive; it must fail too.
    const int t = ::open(f->path().c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    EXPECT_LT(t, 0);
    if (t >= 0) ::close(t);

    EXPECT_EQ(readFile(f->path()), "GENUINE-PRODUCER-BYTES");
    std::error_code ec;
    EXPECT_TRUE(f->verifyUnchanged(ec)) << ec.message();
}

TEST(TempFileSafetyPhase60, PosixUntouchedSealedFileVerifiesAndUnsealedDoesNot)
{
    // Vacuity guard: a verifier that always says "changed" would pass every
    // detection case below.
    auto f = producedAndSealed("pdfe_seal_ok", "UNTOUCHED");
    ASSERT_TRUE(f);
    std::error_code ec;
    EXPECT_TRUE(f->verifyUnchanged(ec)) << ec.message();
    EXPECT_TRUE(f->verifyUnchanged(ec)) << "verification must be repeatable";
    EXPECT_EQ(readFile(f->path()), "UNTOUCHED");

    // And an UNSEALED file must not verify: there is no snapshot to compare.
    auto u = PinnedTempFile::reserve(tempDir(), "pdfe_unsealed", ".bin", ec);
    ASSERT_TRUE(u) << ec.message();
    EXPECT_FALSE(u->isSealed());
    EXPECT_FALSE(u->verifyUnchanged(ec));
}

TEST(TempFileSafetyPhase60, PosixRenameOverASealedFileIsDetected)
{
    auto f = producedAndSealed("pdfe_seal_rename", "PINNED-CONTENT");
    ASSERT_TRUE(f);
    const fs::path decoy = f->path().parent_path()
                           / (f->path().filename().string() + ".decoy");
    // Same size as the original, so a size-only check would miss it.
    ASSERT_TRUE(writeFile(decoy, "ATTACKER-BYTES"));
    ASSERT_EQ(::rename(decoy.c_str(), f->path().c_str()), 0)
        << "POSIX permits the rename-over (no share modes) — that is the gap "
           "this case documents";
    EXPECT_EQ(readFile(f->path()), "ATTACKER-BYTES");
    std::error_code ec;
    EXPECT_FALSE(f->verifyUnchanged(ec))
        << "a rename-over of the pinned name was NOT detected";
}

TEST(TempFileSafetyPhase60, PosixInPlaceWriteAfterSealIsDetected)
{
    // A deliberate same-user attacker: chmod the file back, overwrite it in
    // place with same-size bytes (same inode, so an identity check alone sees
    // nothing), then restore 0400 to cover the tracks.
    auto f = producedAndSealed("pdfe_seal_inplace", "GENUINE-BYTES!");
    ASSERT_TRUE(f);
    ASSERT_EQ(::chmod(f->path().c_str(), 0600), 0);
    const int w = ::open(f->path().c_str(), O_WRONLY | O_CLOEXEC);
    ASSERT_GE(w, 0);
    const std::string evil = "ATTACKER-BYTES";
    ASSERT_EQ(evil.size(), std::string("GENUINE-BYTES!").size());
    ASSERT_EQ(::write(w, evil.data(), evil.size()), static_cast<ssize_t>(evil.size()));
    ::close(w);
    ASSERT_EQ(::chmod(f->path().c_str(), 0400), 0);

    std::error_code ec;
    EXPECT_FALSE(f->verifyUnchanged(ec))
        << "an in-place same-size rewrite after seal was NOT detected";
}

TEST(TempFileSafetyPhase60, PosixChmodAloneAfterSealIsDetected)
{
    // The first step of every in-place attack. Detected even if no byte moves.
    auto f = producedAndSealed("pdfe_seal_chmod", "BYTES");
    ASSERT_TRUE(f);
    ASSERT_EQ(::chmod(f->path().c_str(), 0600), 0);
    std::error_code ec;
    EXPECT_FALSE(f->verifyUnchanged(ec));
}

TEST(TempFileSafetyPhase60, PosixDeleteOfASealedFileIsDetected)
{
    auto f = producedAndSealed("pdfe_seal_delete", "BYTES");
    ASSERT_TRUE(f);
    ASSERT_EQ(::unlink(f->path().c_str()), 0);
    std::error_code ec;
    EXPECT_FALSE(f->verifyUnchanged(ec));
    // Delete-and-recreate at the same name is a different inode.
    ASSERT_TRUE(writeFile(f->path(), "BYTES"));
    EXPECT_FALSE(f->verifyUnchanged(ec));
    fs::remove(f->path(), ec);
}
#endif

// -----------------------------------------------------------------------------
// The fixed-name write — plant-and-redirect, not substitution.
// -----------------------------------------------------------------------------

#if defined(_WIN32)
TEST(TempFileSafetyPhase60, AFixedNameWriteRefusesAPlantedHardlink)
{
    const fs::path victim = tempDir() / "pdfe_p60_victim.txt";
    const fs::path planted = tempDir() / "pdfe_p60_planted.txt";
    std::error_code ec;
    fs::remove(victim, ec);
    fs::remove(planted, ec);

    ASSERT_TRUE(writeFile(victim, "IMPORTANT USER DATA"));
    // An unprivileged attacker needs no special right for this — a hardlink is
    // available to anyone who can read the victim and write the directory.
    ASSERT_NE(::CreateHardLinkW(planted.wstring().c_str(),
                                victim.wstring().c_str(), nullptr), 0)
        << "could not create the hardlink this case is about (err "
        << ::GetLastError() << ")";

    EXPECT_FALSE(pdfe::core::writeFixedNameTempFile(
        planted, "STATUS_OVERWRITE", /*append=*/false, ec))
        << "the write went through a hardlink — a status sentinel just "
           "truncated a file the user owns";
    EXPECT_EQ(readFile(victim), "IMPORTANT USER DATA")
        << "the victim's contents were destroyed";

    fs::remove(planted, ec);
    fs::remove(victim, ec);
}

TEST(TempFileSafetyPhase60, AFixedNameWriteIsNotRedirectedByAPlantedSymlink)
{
    // Either outcome is a pass, and both are real:
    //   * the attacker cannot create a file symlink at all without
    //     SeCreateSymbolicLinkPrivilege / Developer Mode, so the primitive is
    //     unavailable to them on a default host; or
    //   * they can, and FILE_FLAG_OPEN_REPARSE_POINT means our open lands on the
    //     link object rather than following it to the victim.
    //
    // LIMIT, STATED: that flag governs the FINAL component only. A junction
    // planted on a PARENT directory is still traversed, and this case does not
    // claim otherwise. The defence there is that %TEMP% is per-user.
    const fs::path victim = tempDir() / "pdfe_p60_sym_victim.txt";
    const fs::path link = tempDir() / "pdfe_p60_sym_link.txt";
    std::error_code ec;
    fs::remove(victim, ec);
    fs::remove(link, ec);
    ASSERT_TRUE(writeFile(victim, "IMPORTANT USER DATA"));

    const BOOLEAN made = ::CreateSymbolicLinkW(
        link.wstring().c_str(), victim.wstring().c_str(),
        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    if (!made) {
        const DWORD err = ::GetLastError();
        EXPECT_TRUE(err == ERROR_PRIVILEGE_NOT_HELD || err == ERROR_INVALID_PARAMETER)
            << "symlink creation failed for an unexpected reason (" << err
            << "), so this case established nothing";
    } else {
        EXPECT_FALSE(pdfe::core::writeFixedNameTempFile(
            link, "STATUS_OVERWRITE", /*append=*/false, ec))
            << "the write followed a planted symlink";
        EXPECT_EQ(readFile(victim), "IMPORTANT USER DATA");
    }
    fs::remove(link, ec);
    fs::remove(victim, ec);
}

TEST(TempFileSafetyPhase60, AFixedNameWriteStillWorksOnAnHonestPath)
{
    // The vacuity guard for the two cases above: a refusal-only implementation
    // would pass both of them and silently retire the UI-automation sentinel
    // eight Python tests poll.
    const fs::path path = tempDir() / "pdfe_p60_honest.txt";
    std::error_code ec;
    fs::remove(path, ec);
    ASSERT_TRUE(pdfe::core::writeFixedNameTempFile(
        path, "TEXT_EDIT_ACTIVATING\n", /*append=*/false, ec)) << ec.message();
    EXPECT_EQ(readFile(path), "TEXT_EDIT_ACTIVATING\n");

    // And it truncates rather than appending, which is what the sentinel's
    // pollers depend on.
    ASSERT_TRUE(pdfe::core::writeFixedNameTempFile(path, "OK\n", false, ec));
    EXPECT_EQ(readFile(path), "OK\n");
    fs::remove(path, ec);
}
#else  // POSIX arms of the three fixed-name cases (Linux port, phase 6)
TEST(TempFileSafetyPhase60, AFixedNameWriteRefusesAPlantedHardlinkPosix)
{
    const fs::path victim = tempDir() / "pdfe_p60_victim.txt";
    const fs::path planted = tempDir() / "pdfe_p60_planted.txt";
    std::error_code ec;
    fs::remove(victim, ec);
    fs::remove(planted, ec);
    ASSERT_TRUE(writeFile(victim, "IMPORTANT USER DATA"));
    ASSERT_EQ(::link(victim.c_str(), planted.c_str()), 0)
        << "could not create the hardlink this case is about (errno " << errno << ")";

    EXPECT_FALSE(pdfe::core::writeFixedNameTempFile(
        planted, "STATUS_OVERWRITE", /*append=*/false, ec))
        << "the write went through a hardlink";
    EXPECT_EQ(readFile(victim), "IMPORTANT USER DATA")
        << "the victim's contents were destroyed (truncated before the check?)";
    fs::remove(planted, ec);
    fs::remove(victim, ec);
}

TEST(TempFileSafetyPhase60, AFixedNameWriteIsNotRedirectedByAPlantedSymlinkPosix)
{
    // On POSIX an unprivileged user CAN plant a symlink, so unlike the Windows
    // case there is no "primitive unavailable" branch: the refusal must hold.
    const fs::path victim = tempDir() / "pdfe_p60_sym_victim.txt";
    const fs::path link = tempDir() / "pdfe_p60_sym_link.txt";
    std::error_code ec;
    fs::remove(victim, ec);
    fs::remove(link, ec);
    ASSERT_TRUE(writeFile(victim, "IMPORTANT USER DATA"));
    ASSERT_EQ(::symlink(victim.c_str(), link.c_str()), 0);

    EXPECT_FALSE(pdfe::core::writeFixedNameTempFile(
        link, "STATUS_OVERWRITE", /*append=*/false, ec))
        << "the write followed a planted symlink";
    EXPECT_EQ(readFile(victim), "IMPORTANT USER DATA");
    fs::remove(link, ec);
    fs::remove(victim, ec);
}

TEST(TempFileSafetyPhase60, AFixedNameWriteRefusesAPlantedFifoWithoutHanging)
{
    const fs::path fifo = tempDir() / "pdfe_p60_fifo.txt";
    std::error_code ec;
    fs::remove(fifo, ec);
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    // O_NONBLOCK open of a FIFO with no reader fails (ENXIO) rather than
    // blocking; either way the write must be refused, not hang the UI thread.
    EXPECT_FALSE(pdfe::core::writeFixedNameTempFile(
        fifo, "STATUS", /*append=*/false, ec));
    fs::remove(fifo, ec);
}

TEST(TempFileSafetyPhase60, AFixedNameWriteStillWorksOnAnHonestPathPosix)
{
    const fs::path path = tempDir() / "pdfe_p60_honest.txt";
    std::error_code ec;
    fs::remove(path, ec);
    ASSERT_TRUE(pdfe::core::writeFixedNameTempFile(
        path, "TEXT_EDIT_ACTIVATING\n", /*append=*/false, ec)) << ec.message();
    EXPECT_EQ(readFile(path), "TEXT_EDIT_ACTIVATING\n");
    struct stat st {};
    ASSERT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 077, 0u) << "the sentinel was created group/other-accessible";

    ASSERT_TRUE(pdfe::core::writeFixedNameTempFile(path, "OK\n", false, ec));
    EXPECT_EQ(readFile(path), "OK\n") << "a non-append write must truncate";
    ASSERT_TRUE(pdfe::core::writeFixedNameTempFile(path, "MORE\n", true, ec));
    EXPECT_EQ(readFile(path), "OK\nMORE\n") << "append mode must append";
    fs::remove(path, ec);
}
#endif

// -----------------------------------------------------------------------------
// The diagnostics root — acceptance criteria 2 and 5.
// -----------------------------------------------------------------------------

TEST(TempFileSafetyPhase60, DiagnosticsRootIsUnpredictableStableAndInsideTemp)
{
    const fs::path& a = pdfe::core::diagnosticsDir();
    ASSERT_FALSE(a.empty()) << "the diagnostics root could not be created";
    const fs::path& b = pdfe::core::diagnosticsDir();
    EXPECT_EQ(a, b) << "the root must be created once per process, not per call";
    EXPECT_TRUE(fs::exists(a));

    const std::string leaf = a.filename().string();
    ASSERT_EQ(leaf.rfind("pdfe_diag.", 0), 0u) << leaf;
    EXPECT_EQ(leaf.size(), std::string("pdfe_diag.").size() + 32u)
        << "the root's name is not 32 hex digits: " << leaf;

    // Inside %TEMP% — not the process CWD, which is what GlyphAtlas's
    // "debug_log.txt" resolved against before this phase.
    // lexically_normal on both sides: fs::temp_directory_path() returns a
    // trailing-separator form on Windows, so a raw == compares "…\Temp\1" with
    // "…\Temp\1\" and fails for a reason that has nothing to do with the
    // property under test.
    EXPECT_EQ(a.parent_path().lexically_normal(),
              (tempDir() / "x").parent_path().lexically_normal());

    const fs::path sub = pdfe::core::diagnosticsSubdir("pdfe_glyph_debug");
    ASSERT_FALSE(sub.empty());
    EXPECT_EQ(sub.parent_path(), a);
    EXPECT_TRUE(fs::is_directory(sub));
}

TEST(TempFileSafetyPhase60, DiagnosticsSubdirRefusesToEscapeItsRoot)
{
    // Without this the "unpredictable root" property is one caller away from
    // being nothing at all.
    for (const char* escape : {"..", "../evil", R"(..\evil)", "C:/evil", "a/b"}) {
        EXPECT_TRUE(pdfe::core::diagnosticsSubdir(escape).empty())
            << "diagnosticsSubdir accepted a path component that escapes: "
            << escape;
    }
    EXPECT_TRUE(pdfe::core::diagnosticsSubdir("").empty());
}

// -----------------------------------------------------------------------------
// The product sites actually route through the primitives.
//
// A primitive nothing calls proves nothing — the failure mode this project has
// hit repeatedly is a hardened mechanism sitting beside an unmigrated call site.
// -----------------------------------------------------------------------------

TEST(TempFileSafetyPhase60, NoProductSiteStillDerivesATempNameFromAnAddressOrCounter)
{
    struct Site {
        const char* file;
        const char* mustNotContain;
        const char* why;
    };
    const Site sites[] = {
        {"src/ui/CombineFilesService.cpp", "reinterpret_cast<std::uintptr_t>",
         "the image wrapper's name was an object ADDRESS plus a sequential index"},
        {"src/ui/CombineFilesService.cpp", "pdfe_combine_img_",
         "the old predictable name shape must be gone, not merely unused"},
        {"src/ui/MainWindow.cpp", "pdfe_clip_paste_%1",
         "the pasted image's name was a counter that restarts at 0 each process"},
        {"src/tre/GlyphAtlas.cpp", "QStringLiteral(\"debug_log.txt\")",
         "GlyphAtlas wrote relative to the process CWD, not to %TEMP% at all"},
    };
    for (const auto& s : sites) {
        const std::string src = stripComments(readFile(repoRoot() / s.file));
        ASSERT_FALSE(src.empty()) << "could not read " << s.file;
        EXPECT_EQ(src.find(s.mustNotContain), std::string::npos)
            << s.file << " still contains \"" << s.mustNotContain << "\": " << s.why;
    }
}

TEST(TempFileSafetyPhase60, EveryMigratedSiteRoutesThroughTheHardenedPrimitive)
{
    struct Site { const char* file; const char* mustContain; };
    const Site sites[] = {
        {"src/ui/CombineFilesService.cpp", "PinnedTempFile::reserve"},
        {"src/ui/CombineFilesService.cpp", "->seal("},
        {"src/ui/CombineFilesService.cpp", "->verifyUnchanged("},
        {"src/ui/MainWindow.cpp", "PinnedTempFile::reserve"},
        {"src/ui/MainWindow.cpp", "->seal("},
        {"src/ui/MainWindow.cpp", "->verifyUnchanged("},
        {"src/ui/MainWindow.cpp", "writeFixedNameTempFile"},
        {"src/tre/GlyphAtlas.cpp", "diagnosticsSubdir"},
        {"src/tre/FontMatcher.cpp", "diagnosticsSubdir"},
        {"src/tre/EnrichedAtlas.cpp", "diagnosticsSubdir"},
        {"src/tre/CNNFontRanker.cpp", "diagnosticsSubdir"},
    };
    for (const auto& s : sites) {
        const std::string src = stripComments(readFile(repoRoot() / s.file));
        ASSERT_FALSE(src.empty()) << "could not read " << s.file;
        EXPECT_NE(src.find(s.mustContain), std::string::npos)
            << s.file << " no longer calls " << s.mustContain
            << " — a migrated site was reverted or moved";
    }
}

TEST(TempFileSafetyPhase60, ThereIsExactlyOneCsprngImplementationInTheProduct)
{
    // AtomicFileReplace's randomHex128 used to be a second BCryptGenRandom call
    // site, and phase-60's first instinct was to copy it a third time. Two
    // implementations of a security primitive is two chances to drift, and only
    // one of them gets reviewed.
    std::vector<std::string> callSites;
    for (const auto& entry : fs::recursive_directory_iterator(repoRoot() / "src")) {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h") continue;
        const std::string src = stripComments(readFile(entry.path()));
        if (src.find("BCryptGenRandom") != std::string::npos) {
            callSites.push_back(entry.path().filename().string());
        }
    }
    ASSERT_FALSE(callSites.empty())
        << "no BCryptGenRandom call site found at all — the walk is broken, and "
           "a broken walk would pass this as compliance";
    EXPECT_EQ(callSites.size(), 1u)
        << "expected exactly one CSPRNG implementation (TempFileSafety.cpp); found "
        << callSites.size();
    EXPECT_EQ(callSites.front(), "TempFileSafety.cpp");
}
