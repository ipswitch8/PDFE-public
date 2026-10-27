// -*- coding: utf-8 -*-
// Negative-path save coverage (pipeline e5b9c231 Phase 4, Register M-d /
// rtm-coverage.md "Save error paths — HAPPY-PATH-ONLY"). A save that cannot
// complete (read-only target, target is a directory, missing parent directory,
// or an externally-locked target) must surface a CLEAR error, never silently
// "succeed" and lose the user's changes. This is the failure class behind the
// file-lock lost-changes defect in project memory.
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

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
