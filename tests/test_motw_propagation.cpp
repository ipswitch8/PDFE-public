// -*- coding: utf-8 -*-
//
// GitHub #107 (WIN-96) -- Mark-of-the-Web survives Save As.
//
// Windows records provenance in a `Zone.Identifier` alternate data stream;
// `ZoneId=3` means "came from the internet", and SmartScreen, Office Protected
// View and the shell all act on it.
//
// THE DEFECT. PDFE already WROTE a mark on files it extracts from a document
// (AttachmentManager::markOfTheWeb, covered by
// ExtractPolicyTest.ExtractedFilesCarryMarkOfTheWeb). What it did not do was
// PROPAGATE an existing mark on Save As. Open a PDF downloaded from the
// internet, save it under a new name, and the copy carried no mark at all --
// a silent provenance downgrade performed by the editor.
//
// A CORRECTION TO THE ISSUE'S FRAMING, worth recording because it was my own
// earlier claim: #107's repaired body says Mark-of-the-Web is "absent
// tree-wide". That is NOT true -- the attachment path has had it, with a test,
// since phase-71. What was absent is propagation on save. The narrower
// statement is the accurate one and is what these tests pin.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/ZoneIdentifier.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using namespace pdfe::core;

namespace {

fs::path fixtures() { return fs::path(TEST_FIXTURES_DIR); }
fs::path sourcePdf() { return fixtures() / "test_single.pdf"; }

std::shared_ptr<MuPdfContext> ctx() { return std::make_shared<MuPdfContext>(); }

/// Write an internet-zone mark onto `file`, the way a browser download would.
bool writeInternetZone(const fs::path& file)
{
#if defined(_WIN32)
    const std::wstring ads = file.wstring() + L":Zone.Identifier";
    std::ofstream z(ads, std::ios::binary | std::ios::trunc);
    if (!z) return false;
    z << "[ZoneTransfer]\r\nZoneId=3\r\n";
    return static_cast<bool>(z);
#else
    (void)file;
    return false;
#endif
}

fs::path workDir(const char* tag)
{
    std::error_code ec;
    fs::path d = fs::temp_directory_path() / (std::string("pdfe_motw_") + tag);
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

}  // namespace

#if defined(_WIN32)

// The core defect: a marked source must produce a marked destination.
TEST(MotwPropagation, SaveAsCarriesTheSourceMarkToTheDestination)
{
    const fs::path dir = workDir("carry");
    const fs::path src = dir / "downloaded.pdf";

    std::error_code ec;
    fs::copy_file(sourcePdf(), src, fs::copy_options::overwrite_existing, ec);
    ASSERT_TRUE(fs::exists(src)) << "could not stage the fixture: " << ec.message();

    ASSERT_TRUE(writeInternetZone(src))
        << "could not write a Zone.Identifier on the source. If this "
           "filesystem has no alternate data stream support the whole feature "
           "is inapplicable here -- but then this test must FAIL loudly rather "
           "than skip, because a silent skip is how the gap survived before.";

    ASSERT_TRUE(readZoneIdentifier(src).has_value())
        << "the mark we just wrote is not readable back; the test's own "
           "premise is broken";

    const fs::path dst = dir / "saved-elsewhere.pdf";
    {
        auto c = ctx();
        auto doc = Document::open(c, src);
        ASSERT_NE(doc, nullptr);
        ASSERT_NO_THROW(doc->saveAs(dst));
    }
    ASSERT_TRUE(fs::exists(dst)) << "saveAs wrote nothing";

    const auto mark = readZoneIdentifier(dst);
    ASSERT_TRUE(mark.has_value())
        << "the saved copy carries NO Mark-of-the-Web. A file the platform "
           "knew came from the internet has become one it believes was "
           "authored locally -- SmartScreen will not inspect it and Office "
           "will not open it in Protected View.";

    EXPECT_NE(mark->find("ZoneId=3"), std::string::npos)
        << "a Zone.Identifier exists but does not carry ZoneId=3; the "
           "provenance was altered rather than preserved. Got: " << *mark;
}

// The other half of "propagate", and the reason this is not simply "always
// mark": an unmarked source must NOT gain a mark. If every saved document were
// stamped ZoneId=3 they would all open in Protected View and the mark would
// stop carrying information.
TEST(MotwPropagation, SaveAsDoesNotInventAMarkForAnUnmarkedSource)
{
    const fs::path dir = workDir("nomark");
    const fs::path src = dir / "authored-locally.pdf";

    std::error_code ec;
    fs::copy_file(sourcePdf(), src, fs::copy_options::overwrite_existing, ec);
    ASSERT_TRUE(fs::exists(src));
    ASSERT_FALSE(readZoneIdentifier(src).has_value())
        << "the freshly copied fixture already carries a mark; this test's "
           "premise (an unmarked source) does not hold";

    const fs::path dst = dir / "saved-locally.pdf";
    {
        auto c = ctx();
        auto doc = Document::open(c, src);
        ASSERT_NE(doc, nullptr);
        ASSERT_NO_THROW(doc->saveAs(dst));
    }

    EXPECT_FALSE(readZoneIdentifier(dst).has_value())
        << "an unmarked source produced a MARKED destination. Marking "
           "everything makes the mark meaningless and would put every saved "
           "document into Protected View.";
}

// The utility's own contract, asserted directly so the three outcomes are
// distinguishable. A bare bool would conflate "nothing to copy" with "tried
// and failed", and a silent failure here is a silent security downgrade.
TEST(MotwPropagation, PropagationReportsWhichOutcomeOccurred)
{
    const fs::path dir = workDir("outcomes");

    const fs::path unmarked = dir / "a.pdf";
    const fs::path marked   = dir / "b.pdf";
    const fs::path target   = dir / "c.pdf";

    std::error_code ec;
    fs::copy_file(sourcePdf(), unmarked, fs::copy_options::overwrite_existing, ec);
    fs::copy_file(sourcePdf(), marked,   fs::copy_options::overwrite_existing, ec);
    fs::copy_file(sourcePdf(), target,   fs::copy_options::overwrite_existing, ec);
    ASSERT_TRUE(fs::exists(unmarked) && fs::exists(marked) && fs::exists(target));

    EXPECT_EQ(propagateZoneIdentifier(unmarked, target),
              ZonePropagation::NoSourceMark);

    ASSERT_TRUE(writeInternetZone(marked));
    EXPECT_EQ(propagateZoneIdentifier(marked, target),
              ZonePropagation::Copied);
    EXPECT_TRUE(readZoneIdentifier(target).has_value());
}

// Propagation must not REMOVE a mark the destination already has when the
// source has none -- that is a downgrade in the other direction. The contract
// is to preserve provenance, not to normalise it.
TEST(MotwPropagation, AnUnmarkedSourceDoesNotStripAnExistingDestinationMark)
{
    const fs::path dir = workDir("nostrip");
    const fs::path unmarked = dir / "src.pdf";
    const fs::path target   = dir / "dst.pdf";

    std::error_code ec;
    fs::copy_file(sourcePdf(), unmarked, fs::copy_options::overwrite_existing, ec);
    fs::copy_file(sourcePdf(), target,   fs::copy_options::overwrite_existing, ec);
    ASSERT_TRUE(writeInternetZone(target));
    ASSERT_TRUE(readZoneIdentifier(target).has_value());

    EXPECT_EQ(propagateZoneIdentifier(unmarked, target),
              ZonePropagation::NoSourceMark);
    EXPECT_TRUE(readZoneIdentifier(target).has_value())
        << "propagating from an unmarked source STRIPPED the destination's "
           "existing mark";
}

#endif  // _WIN32
