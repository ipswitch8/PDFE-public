// -*- coding: utf-8 -*-
//
// Extraction-permission gate on the on-disk (Tantivy) search index.
//
// pdfe::search::DocumentSearchIndex writes every page's plain text into a
// Tantivy index under std::filesystem::temp_directory_path(). For a
// copy-restricted / encrypted document that is a plaintext-searchable copy of
// the content living OUTSIDE the encrypted container, readable by any process
// with temp-dir access until the destructor removes it — the same
// confidentiality class as the accessibility-tree bypass closed in
// src/ui/PdfCanvasAccessible.cpp, and it takes the same predicate:
//
//     permitted  <=>  extractForAccessibilityAllowed() || copyAllowed()
//                     (ISO 32000-1 Table 22 bit 10, bit 5)
//
// Only a document denying BOTH is refused. The refusal is a silent no-op — no
// directory, no index, built_ stays false — never a throw, because ensureBuilt()
// sits on the UI's hot search path. Search itself keeps working: SearchPanel
// falls back to the purely in-process pdfe::core::SearchEngine.
//
// The oracle for the denied case is the ABSENCE OF THE INDEX DIRECTORY, not
// merely an empty result list: an empty searchPages() would also be produced by
// a fully-written index that simply had no hit for the query, which is exactly
// the leak we are testing for. We additionally sweep whatever bytes exist under
// the index dir for the document's distinctive tokens.
//
// The controls are what make this a real discrimination rather than a blanket
// off-switch: an implementation that refused everything would fail
// UnencryptedDocumentStillBuildsIndexAndFindsHits and
// CopyDeniedButAccessibilityAllowedStillIndexes.
//
// Revision note: QPDF refuses to CLEAR bit 10 for R > 3 (PDF 2.0 deprecates it)
// — asserted by PermissionBitsRoundTrip.AccessibilityBitIsNotExpressibleAboveR3
// — so the denied-both fixture MUST be written at R3 (RC4-128).

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"
#include "search/DocumentSearchIndex.h"
#include "search/TantivyIndex.h"

#include <QTemporaryDir>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::encryption;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::search::DocumentSearchIndex;

namespace {

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// Distinctive text baked into tests/fixtures/test_3page.pdf. "title" is the
// token a Tantivy default tokenizer would store for "Title"; searching for the
// lowercased form catches the leak whether the bytes are stored raw or
// tokenized.
const char* kQueryTerm = "title";
const char* kRawMarker = "Page 1 - Title Page";

/// Encrypt test_3page.pdf with the given algorithm/permissions. Empty user
/// password so the reopen observes the RESTRICTED (user-level) view — opening
/// with the owner password would grant everything and make the gate vacuous.
std::filesystem::path writeEncrypted(const std::filesystem::path& dir,
                                     const std::string& name,
                                     EncryptionAlgorithm algorithm,
                                     const PermissionFlags& perms) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto src = Document::open(ctx, fixturesDir() / "test_3page.pdf");
    EXPECT_NE(src, nullptr);
    EncryptionManager mgr(ctx);

    EncryptionSettings settings;
    settings.algorithm = algorithm;
    settings.userPassword = "";
    settings.ownerPassword = "ownerpw";
    settings.permissions = perms;

    auto out = dir / name;
    mgr.saveEncrypted(src.get(), out, settings);
    EXPECT_TRUE(std::filesystem::exists(out));
    return out;
}

std::string lowered(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

struct DirScan {
    bool exists = false;
    std::uintmax_t fileCount = 0;
    std::uintmax_t totalBytes = 0;
    std::vector<std::string> filesContainingMarker;
};

/// Walk `dir` and report what is on disk, plus which files contain any of the
/// document's distinctive tokens (case-insensitive raw-byte scan — the strong
/// oracle: it does not care how Tantivy chose to serialise the text).
DirScan scanForLeakedText(const std::filesystem::path& dir) {
    DirScan scan;
    std::error_code ec;
    scan.exists = std::filesystem::exists(dir, ec);
    if (!scan.exists) return scan;

    const std::string needleTok = lowered(kQueryTerm);
    const std::string needleRaw = lowered(kRawMarker);

    for (std::filesystem::recursive_directory_iterator it(dir, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        scan.fileCount++;
        scan.totalBytes += std::filesystem::file_size(it->path(), ec);

        std::ifstream in(it->path(), std::ios::binary);
        if (!in) continue;
        std::string bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
        const std::string hay = lowered(bytes);
        if (hay.find(needleTok) != std::string::npos ||
            hay.find(needleRaw) != std::string::npos) {
            scan.filesContainingMarker.push_back(it->path().string());
        }
    }
    return scan;
}

} // namespace

class SearchIndexPermissionGateTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(std::filesystem::exists(fixturesDir() / "test_3page.pdf"))
            << "test_3page.pdf fixture is required";
        ASSERT_TRUE(tmp_.isValid());
    }

    std::filesystem::path dir() const {
        return std::filesystem::path(tmp_.path().toStdString());
    }

    QTemporaryDir tmp_;
};

// ===========================================================================
// The defect: copy DENIED and accessibility DENIED -> no on-disk index at all.
// ===========================================================================

TEST_F(SearchIndexPermissionGateTest, CopyAndAccessibilityDeniedWritesNoIndexToDisk) {
    PermissionFlags perms = PermissionFlags::all();
    perms.copy = false;
    perms.accessibility = false;
    // R3 (RC4-128) is the newest revision whose writer honours bit 10.
    auto path = writeEncrypted(dir(), "deny_both_r3.pdf",
                               EncryptionAlgorithm::RC4_128, perms);

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(doc->copyAllowed())
        << "fixture must actually deny copy";
    ASSERT_FALSE(doc->extractForAccessibilityAllowed())
        << "fixture must actually deny accessibility extraction (R3)";
    // The document really does still hold the text that could leak — otherwise
    // an absent index below would prove nothing.
    ASSERT_FALSE(doc->page(0)->extractPlainText().empty());
    ASSERT_NE(doc->page(0)->extractPlainText().find("Title"), std::string::npos);

    DocumentSearchIndex idx(doc.get());
    const auto indexDir = idx.indexDir();
    // Each fixture lives in a unique QTemporaryDir, and the index dir is keyed
    // by a hash of the document path, so nothing can pre-exist here.
    ASSERT_FALSE(std::filesystem::exists(indexDir))
        << "precondition: index dir must not pre-exist at " << indexDir.string();

    // Drive every entry point that could materialise text on disk.
    std::vector<pdfe::search::SearchHit> hits;
    EXPECT_NO_THROW({ hits = idx.searchPages(kQueryTerm, 100); })
        << "the refusal must be silent — searchPages() is on the UI hot path";
    EXPECT_NO_THROW({ idx.build(doc.get()); });
    EXPECT_NO_THROW({ idx.reindexPage(0); });
    EXPECT_NO_THROW({ idx.invalidatePage(0); });
    idx.waitForPendingReindex();

    // --- The strong oracle: nothing was written outside the container. ---
    const DirScan scan = scanForLeakedText(indexDir);
    EXPECT_FALSE(scan.exists)
        << "a Tantivy index directory was created for a copy-denied + "
           "accessibility-denied document at " << indexDir.string()
        << " (" << scan.fileCount << " files, " << scan.totalBytes << " bytes)";
    EXPECT_EQ(scan.fileCount, 0u)
        << "no index files may exist for a document denying extraction";
    EXPECT_TRUE(scan.filesContainingMarker.empty())
        << "document text leaked into the on-disk index: "
        << (scan.filesContainingMarker.empty()
                ? std::string{}
                : scan.filesContainingMarker.front());

    // --- And the in-memory state agrees: the index was never built. ---
    EXPECT_EQ(idx.documentCount(), 0)
        << "the index must not hold any documents";
    EXPECT_TRUE(hits.empty());
    EXPECT_TRUE(idx.query(kQueryTerm).empty());
}

// ===========================================================================
// CONTROL 1 — an unencrypted document must still build its index AND find
// hits. Without this, "refuse everything" passes the test above.
// ===========================================================================

TEST_F(SearchIndexPermissionGateTest, UnencryptedDocumentStillBuildsIndexAndFindsHits) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixturesDir() / "test_3page.pdf");
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(doc->copyAllowed());
    ASSERT_TRUE(doc->extractForAccessibilityAllowed());

    DocumentSearchIndex idx(doc.get());
    idx.invalidate();  // start from a clean on-disk state
    auto hits = idx.searchPages(kQueryTerm, 100);

    EXPECT_GT(idx.documentCount(), 0)
        << "the gate must not disable indexed search for unrestricted documents";
    EXPECT_FALSE(hits.empty())
        << "unencrypted document must still return indexed search hits";
    EXPECT_TRUE(std::filesystem::exists(idx.indexDir()))
        << "the index directory must be created for an unrestricted document";

    const DirScan scan = scanForLeakedText(idx.indexDir());
    EXPECT_GT(scan.fileCount, 0u)
        << "an unrestricted document's index must actually contain files";
}

// ===========================================================================
// CONTROL 2 — copy DENIED but accessibility ALLOWED must still index. This is
// the shared predicate (bit 10 alone permits), and it is what makes the gate a
// discrimination between the two bits rather than an off-switch keyed on
// copyAllowed() alone.
// ===========================================================================

TEST_F(SearchIndexPermissionGateTest, CopyDeniedButAccessibilityAllowedStillIndexes) {
    PermissionFlags perms = PermissionFlags::all();
    perms.copy = false;               // general extraction denied ...
    perms.accessibility = true;       // ... but assistive tech explicitly allowed
    auto path = writeEncrypted(dir(), "deny_copy_allow_a11y_r3.pdf",
                               EncryptionAlgorithm::RC4_128, perms);

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(doc->copyAllowed())
        << "fixture must actually deny copy — otherwise this control is vacuous";
    ASSERT_TRUE(doc->extractForAccessibilityAllowed());

    DocumentSearchIndex idx(doc.get());
    auto hits = idx.searchPages(kQueryTerm, 100);

    EXPECT_GT(idx.documentCount(), 0)
        << "bit 10 permits extraction even when bit 5 denies general copying";
    EXPECT_FALSE(hits.empty());
    EXPECT_TRUE(std::filesystem::exists(idx.indexDir()));
}

// ===========================================================================
// CONTROL 3 — the modern-encryption reality check. QPDF cannot clear bit 10
// above R3, so an R6 copy-denied document still grants accessibility and must
// still index. If QPDF ever gains that ability this goes RED alongside
// PermissionBitsRoundTrip.AccessibilityBitIsNotExpressibleAboveR3.
// ===========================================================================

TEST_F(SearchIndexPermissionGateTest, EncryptedR6CopyDeniedStillIndexes) {
    PermissionFlags perms = PermissionFlags::all();
    perms.copy = false;
    perms.accessibility = false;   // requested, but not expressible at R6
    auto path = writeEncrypted(dir(), "deny_both_r6.pdf",
                               EncryptionAlgorithm::AES_256, perms);

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(doc->copyAllowed());
    ASSERT_TRUE(doc->extractForAccessibilityAllowed())
        << "R6 is expected to force bit 10 on (PDF 2.0 deprecates clearing it)";

    DocumentSearchIndex idx(doc.get());
    auto hits = idx.searchPages(kQueryTerm, 100);

    EXPECT_GT(idx.documentCount(), 0);
    EXPECT_FALSE(hits.empty());
}
