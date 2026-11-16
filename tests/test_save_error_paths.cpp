// -*- coding: utf-8 -*-
// Negative-path save coverage (pipeline e5b9c231 Phase 4, Register M-d /
// rtm-coverage.md "Save error paths — HAPPY-PATH-ONLY"). A save that cannot
// complete (read-only target, target is a directory, missing parent directory,
// or an externally-locked target) must surface a CLEAR error, never silently
// "succeed" and lose the user's changes. This is the failure class behind the
// file-lock lost-changes defect in project memory.
#include <gtest/gtest.h>
#include "core/AtomicFileReplace.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace pdfe::core;

namespace {
std::filesystem::path fixtures() { return std::filesystem::path(TEST_FIXTURES_DIR); }

std::shared_ptr<MuPdfContext> ctx() { return std::make_shared<MuPdfContext>(); }

std::unique_ptr<Document> openClean(std::shared_ptr<MuPdfContext> c) {
    return Document::open(c, fixtures() / "test_single.pdf");
}
} // namespace

// A save whose PARENT directory does not exist must throw, not silently drop
// the changes.
TEST(SaveErrorPaths, MissingParentDirectoryThrows) {
    auto c = ctx();
    auto doc = openClean(c);
    ASSERT_NE(doc, nullptr);
    auto bad = std::filesystem::temp_directory_path() /
               "pdfe_nonexistent_dir_xyz" / "out.pdf";
    std::error_code ec;
    std::filesystem::remove_all(bad.parent_path(), ec);  // ensure absent
    EXPECT_ANY_THROW(doc->saveAs(bad))
        << "save into a missing directory silently 'succeeded'";
    EXPECT_FALSE(std::filesystem::exists(bad));
}

// A save whose target path is an existing DIRECTORY must throw.
TEST(SaveErrorPaths, TargetIsADirectoryThrows) {
    auto c = ctx();
    auto doc = openClean(c);
    ASSERT_NE(doc, nullptr);
    auto dir = std::filesystem::temp_directory_path() / "pdfe_saveas_dir_target";
    std::filesystem::create_directories(dir);
    EXPECT_ANY_THROW(doc->saveAs(dir))
        << "save onto a directory path silently 'succeeded'";
    // Throwing is half the contract; the other half is that the failed save
    // left the directory alone rather than replacing it with a PDF or emptying
    // it. Without this the test passes against a save that throws AFTER
    // clobbering the target (GitHub #163).
    EXPECT_TRUE(std::filesystem::is_directory(dir))
        << "the failed save replaced the directory target with a file";
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}

// A save onto a READ-ONLY existing file must throw (and must not leave the
// target truncated/empty -> silent data loss).
TEST(SaveErrorPaths, ReadOnlyTargetThrows) {
    auto c = ctx();
    auto doc = openClean(c);
    ASSERT_NE(doc, nullptr);

    auto target = std::filesystem::temp_directory_path() / "pdfe_readonly_target.pdf";
    { std::ofstream f(target, std::ios::binary); f << "%PDF-1.7\nEXISTING-CONTENT\n"; }
    const auto originalSize = std::filesystem::file_size(target);

#ifdef _WIN32
    ::SetFileAttributesW(target.wstring().c_str(), FILE_ATTRIBUTE_READONLY);
#else
    std::filesystem::permissions(target, std::filesystem::perms::owner_read);
#endif

    EXPECT_ANY_THROW(doc->saveAs(target))
        << "save onto a read-only file silently 'succeeded'";
    // The read-only target must be intact -- not truncated to an empty/partial
    // file (which would be silent loss of whatever was there).
    std::error_code ec;
    EXPECT_EQ(std::filesystem::file_size(target, ec), originalSize)
        << "read-only target was clobbered/truncated by a failed save";

#ifdef _WIN32
    ::SetFileAttributesW(target.wstring().c_str(), FILE_ATTRIBUTE_NORMAL);
#else
    std::filesystem::permissions(target, std::filesystem::perms::owner_all);
#endif
    std::filesystem::remove(target, ec);
}

#ifdef _WIN32
// A save onto a target held open EXCLUSIVELY by another handle must throw
// (Windows share-deny). This is the externally-locked-file case.
TEST(SaveErrorPaths, ExternallyLockedTargetThrows) {
    auto c = ctx();
    auto doc = openClean(c);
    ASSERT_NE(doc, nullptr);

    auto target = std::filesystem::temp_directory_path() / "pdfe_locked_target.pdf";
    const std::string originalBytes = "%PDF-1.7\nLOCKED\n";
    { std::ofstream f(target, std::ios::binary); f << originalBytes; }

    // Open with no sharing -> any other open (the save) fails.
    HANDLE h = ::CreateFileW(target.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                             0 /* no share */, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE) << "could not lock the target for the test";

    EXPECT_ANY_THROW(doc->saveAs(target))
        << "save onto an externally-locked file silently 'succeeded'";

    // Release the exclusive handle first — it denies read sharing too, so the
    // byte oracle below cannot run while it is held.
    ::CloseHandle(h);

    // THE ORACLE: a save that could not open its target must not have altered
    // it. "It threw" alone passes just as well against a save that truncates
    // the victim and then reports failure (GitHub #163).
    std::string afterBytes;
    {
        std::ifstream in(target, std::ios::binary);
        afterBytes.assign(std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(afterBytes, originalBytes)
        << "the failed save modified the externally-locked target";

    std::error_code ec; std::filesystem::remove(target, ec);
}
#endif

// ---------------------------------------------------------------------------
// CLASS-LEVEL (DCA 0118, GitHub #385). The two instance tests above each
// pinned ONE destination shape against ONE entry point (Document::saveAs).
// On POSIX the same defect family lived one layer down: the publish
// primitives themselves replaced a destination they had no business
// replacing, because rename() ignores the destination's mode and MuPDF's
// open path remove()s an empty directory. So this sweeps the public publish
// entry points listed in entryPoints() across the unsavable destination shapes
// in DestShape, and asserts the same two-part contract for each cell: the
// publish reports failure, and the destination is byte-for-byte /
// entry-for-entry what it was before. A new entry point is one row in
// entryPoints(); a new shape is a DestShape value plus its shapeName() and
// makeDestination() cases and an entry in kShapes. Shrinking either table is
// caught: the entry-point count has an asserted floor and kShapes' size is a
// static_assert, so dropping a row is a visible, reviewed edit.
// ---------------------------------------------------------------------------
namespace {

enum class DestShape { EmptyDirectory, NonEmptyDirectory, ReadOnlyFile };
constexpr DestShape kShapes[] = {DestShape::EmptyDirectory,
                                 DestShape::NonEmptyDirectory,
                                 DestShape::ReadOnlyFile};
static_assert(std::size(kShapes) >= 3, "a destination shape was dropped from the sweep");

const char* shapeName(DestShape s) {
    switch (s) {
    case DestShape::EmptyDirectory:    return "empty directory";
    case DestShape::NonEmptyDirectory: return "non-empty directory";
    case DestShape::ReadOnlyFile:      return "read-only file";
    }
    return "?";
}

void makeWritable(const std::filesystem::path& p) {
    std::error_code ec;
#ifdef _WIN32
    ::SetFileAttributesW(p.wstring().c_str(), FILE_ATTRIBUTE_NORMAL);
#else
    std::filesystem::permissions(p, std::filesystem::perms::owner_all, ec);
#endif
}

void makeReadOnly(const std::filesystem::path& p) {
#ifdef _WIN32
    ::SetFileAttributesW(p.wstring().c_str(), FILE_ATTRIBUTE_READONLY);
#else
    std::filesystem::permissions(p, std::filesystem::perms::owner_read);
#endif
}

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// What the destination looked like, so "left intact" is an equality check
// rather than a type check (a directory emptied in place is still a directory).
struct Snapshot {
    bool isDirectory = false;
    bool isRegularFile = false;
    std::set<std::string> entries;
    std::string bytes;
    bool operator==(const Snapshot& o) const {
        return isDirectory == o.isDirectory && isRegularFile == o.isRegularFile
            && entries == o.entries && bytes == o.bytes;
    }
};

Snapshot snapshot(const std::filesystem::path& p) {
    Snapshot s;
    std::error_code ec;
    s.isDirectory = std::filesystem::is_directory(p, ec);
    s.isRegularFile = std::filesystem::is_regular_file(p, ec);
    if (s.isDirectory) {
        for (const auto& e : std::filesystem::directory_iterator(p, ec))
            s.entries.insert(e.path().filename().string() + "=" + readAll(e.path()));
    } else if (s.isRegularFile) {
        s.bytes = readAll(p);
    }
    return s;
}

std::filesystem::path makeDestination(const std::filesystem::path& root, DestShape shape) {
    const auto dest = root / "dest.pdf";
    switch (shape) {
    case DestShape::EmptyDirectory:
        std::filesystem::create_directories(dest);
        break;
    case DestShape::NonEmptyDirectory:
        std::filesystem::create_directories(dest);
        { std::ofstream f(dest / "keep.txt", std::ios::binary); f << "user data"; }
        break;
    case DestShape::ReadOnlyFile:
        { std::ofstream f(dest, std::ios::binary); f << "%PDF-1.7\nEXISTING-CONTENT\n"; }
        makeReadOnly(dest);
        break;
    }
    return dest;
}

struct EntryPoint {
    const char* name;
    // Returns true when the publish REPORTED success (throwing counts as failure).
    std::function<bool(const std::filesystem::path& root,
                       const std::filesystem::path& dest)> publish;
};

std::vector<EntryPoint> entryPoints() {
    return {
        {"Document::saveAs",
         [](const std::filesystem::path&, const std::filesystem::path& dest) {
             auto c = ctx();
             auto doc = openClean(c);
             if (!doc) return true;  // fails the cell loudly: nothing was tested
             try { doc->saveAs(dest); return true; } catch (...) { return false; }
         }},
        {"atomicReplaceFile",
         [](const std::filesystem::path& root, const std::filesystem::path& dest) {
             const auto from = root / "incoming.pdf";
             { std::ofstream f(from, std::ios::binary); f << "%PDF-1.7\nNEW\n"; }
             std::error_code ec;
             return pdfe::core::atomicReplaceFile(from, dest, ec);
         }},
        {"StagingFile::commitOver",
         [](const std::filesystem::path&, const std::filesystem::path& dest) {
             std::error_code ec;
             auto s = pdfe::core::StagingFile::createBeside(dest, ec);
             if (!s) return false;
             std::FILE* fp = s->openDuplicateStream(ec);
             if (fp) {
                 static const char body[] = "%PDF-1.7\nNEW\n";
                 std::fwrite(body, 1, sizeof(body) - 1, fp);
                 std::fclose(fp);
             }
             return s->commitOver(dest, ec);
         }},
        {"StagingFile::commitOverWithRetry",
         [](const std::filesystem::path&, const std::filesystem::path& dest) {
             std::error_code ec;
             auto s = pdfe::core::StagingFile::createBeside(dest, ec);
             if (!s) return false;
             std::FILE* fp = s->openDuplicateStream(ec);
             if (fp) {
                 static const char body[] = "%PDF-1.7\nNEW\n";
                 std::fwrite(body, 1, sizeof(body) - 1, fp);
                 std::fclose(fp);
             }
             // Short retry: the destination is permanently unsavable, so every
             // attempt must fail the same way; this only bounds the test's time.
             return s->commitOverWithRetry(dest, ec, 2,
                                           std::chrono::milliseconds(1));
         }},
    };
}

} // namespace

TEST(SaveErrorPaths, NoPublishEntryPointReplacesAnUnsavableDestination) {
    const auto eps = entryPoints();
    ASSERT_GE(eps.size(), 4u) << "an entry point was dropped from the sweep";
    int cells = 0;
    for (const auto& ep : eps) {
        for (DestShape shape : kShapes) {
            SCOPED_TRACE(std::string(ep.name) + " -> " + shapeName(shape));
            const auto root = std::filesystem::temp_directory_path()
                / ("pdfe_dca0118_" + std::to_string(cells));
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            std::filesystem::create_directories(root);
            const auto dest = makeDestination(root, shape);
            const Snapshot before = snapshot(dest);

            const bool reportedSuccess = ep.publish(root, dest);

            EXPECT_FALSE(reportedSuccess)
                << "the publish reported success onto an unsavable destination";
            EXPECT_TRUE(snapshot(dest) == before)
                << "the destination was replaced or altered by the publish";

            if (shape == DestShape::ReadOnlyFile) makeWritable(dest);
            std::filesystem::remove_all(root, ec);
            ++cells;
        }
    }
    EXPECT_EQ(cells, static_cast<int>(eps.size() * std::size(kShapes)))
        << "the sweep did not cover every listed entry point x shape";
}
