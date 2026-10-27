// -*- coding: utf-8 -*-
//
// R3-22 — XMP <-> document-Info metadata cross-sync.
//
// The gap (register.md R3-22): test_metadata_roundtrip.cpp (Info) and
// test_xmp_metadata_writer.cpp (XMP) each pass in ISOLATION; nothing asserts the
// two metadata stores stay consistent after an edit. MetadataManager writes ONLY
// /Info; the only Info->XMP path is inside XmpMetadataWriter::writePdfAIdentification
// (which snapshots Info at write time). So editing Info after an XMP packet exists
// leaves the XMP dc:title STALE — a viewer that prefers XMP shows the old title.
//
// Oracle: establish a consistent baseline (Info title V1, then build an XMP packet
// from it), then EDIT the Info title to V2 and assert the two stores stay
// consistent — the XMP must reflect V2 and must NOT still carry the stale V1.
// This is expected to RED against the current product (no automatic sync); the
// remediation makes an Info metadata edit refresh the XMP dc:* fields it mirrors.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "metadata/MetadataManager.h"
#include "compliance/XmpMetadataWriter.h"
#include "compliance/ComplianceTypes.h"

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::core;
using pdfe::metadata::MetadataManager;
using pdfe::compliance::XmpMetadataWriter;
using pdfe::compliance::PdfALevel;

namespace {

class FidelityR3_22_XmpInfoSync : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        std::filesystem::path dir = TEST_FIXTURES_DIR;
        src = dir / "test_single.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_single.pdf fixture not available";
        }
        out = std::filesystem::temp_directory_path() /
              "pdfe_r3_22_xmp_info_sync.pdf";
        std::error_code ec;
        std::filesystem::remove(out, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(out, ec);
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path src;
    std::filesystem::path out;
};

TEST_F(FidelityR3_22_XmpInfoSync, InfoEditKeepsXmpConsistent) {
    const std::string V1 = "PDFE_TITLE_V1_BASELINE";
    const std::string V2 = "PDFE_TITLE_V2_EDITED";

    MetadataManager mm(context);
    XmpMetadataWriter xw(context);

    // --- Baseline: Info title V1, then build an XMP packet from that Info. ---
    {
        auto doc = Document::open(context, src);
        ASSERT_NE(doc, nullptr);
        mm.writeField(doc.get(), "info:Title", V1);
        // writePdfAIdentification snapshots the current Info into the XMP packet,
        // so after this call both stores agree on V1.
        xw.writePdfAIdentification(doc.get(), PdfALevel::PdfA_2b);
        doc->save(out);
    }

    // Sanity: the baseline XMP really does carry V1 (else the test below is moot).
    {
        auto doc = Document::open(context, out);
        ASSERT_NE(doc, nullptr);
        const std::string xmp = xw.readXmpPacket(doc.get());
        ASSERT_NE(xmp.find(V1), std::string::npos)
            << "baseline XMP packet did not capture the Info title V1; "
               "cannot exercise the sync gap";
    }

    // --- Edit: change ONLY the Info title to V2, save. ---
    {
        auto doc = Document::open(context, out);
        ASSERT_NE(doc, nullptr);
        mm.writeField(doc.get(), "info:Title", V2);
        doc->save(out);
    }

    // --- Assert consistency after the edit. ---
    auto doc = Document::open(context, out);
    ASSERT_NE(doc, nullptr);

    // Info edit must have persisted.
    EXPECT_EQ(mm.readField(doc.get(), "info:Title"), V2)
        << "the Info title edit did not persist";

    // The XMP store must stay consistent with Info: it must reflect the new
    // title and must NOT still carry the stale baseline title.
    const std::string xmp = xw.readXmpPacket(doc.get());
    EXPECT_NE(xmp.find(V2), std::string::npos)
        << "XMP was not updated to the edited title (Info->XMP sync missing)";
    EXPECT_EQ(xmp.find(V1), std::string::npos)
        << "XMP still carries the STALE baseline title after an Info edit "
           "(the two metadata stores diverged)";
}

} // namespace
