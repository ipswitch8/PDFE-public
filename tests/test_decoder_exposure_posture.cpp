// -*- coding: utf-8 -*-
//
// ═════════════════════════════════════════════════════════════════════
// CLASS-LEVEL TEST for DCA 0011 (meta-audit follow-up phase 42).
//
// DCA 0011 is the OpenJPEG heap-buffer-overflow (#50): a malformed JPEG2000
// stream in a user-supplied PDF corrupts the process heap inside the bundled
// openjp2.dll during decode — an RCE-class defect in third-party code that
// MuPDF's setjmp-based fz_try/fz_catch structurally cannot trap. It was fixed
// by gating the JPX decode behind a runtime flag that is OFF by default.
//
// WHY THE EXISTING TESTS ARE INSTANCE-LEVEL. Every JPX test in
// test_external_flaw_corpus.cpp names `images/jpx-bug.pdf` and the JPX gate by
// hand: JpxDecode_OpenRendersGracefully, JpxDecodeGate_DefaultOffAndToggles,
// JpxImage_DetectedByEnumerator, JpxPlaceholderOverlay_GatedByPreference,
// JpxEndToEnd_DefaultOffRendersWithPlaceholder. All five pin THIS codec, THIS
// fixture, THIS flag. Add a second bundled codec with the same exposure — or
// drop a second malformed-decoder fixture into the corpus — and not one of
// them reacts, because each is a hand-written row naming its own subject.
//
// THE CLASS is: *an untrusted, attacker-supplied byte stream reaching a bundled
// third-party decoder at the SHIPPING DEFAULT, with no standing property over
// which decoders are reachable and which fixtures are exercised.* The escape
// was not "we forgot JPEG2000"; it was that nothing in the harness could state
// what the default decode exposure IS, so a change to it was unobservable.
//
// THE CLASS-LEVEL FORM has three parts, each grounded in something that can
// disagree with it rather than in a literal repeated from the product:
//
//   1. CORPUS SWEEP, DIRECTORY-DRIVEN. Every PDF under
//      tests/fixtures/flaw_corpus/images/ must open and render page 0 at the
//      SHIPPING DEFAULT posture without crashing. The row set comes from
//      readdir, not from a hand-written list, so a malformed-decoder fixture
//      added tomorrow is covered the moment it lands — with no test to
//      remember to write. That is the property whose absence let #50 sit in
//      the corpus as a DISABLED_ single case.
//   2. DEFAULT-OFF POSTURE. Every decode gate PDFE exposes must read as
//      SECURE (off) before anything touches it. This is asserted first, on the
//      pristine process state.
//   3. GATE ENUMERATION, PARSED FROM SOURCE. The gate list is read out of
//      src/core/JpxSecurityGate.h rather than remembered, so a second codec
//      gate added there fails this test until it has a default-off row —
//      instead of silently shipping on-by-default.
//
// RED-ness: flip the JPX gate's default to enabled and part 2 fails and part 1
// crashes the shard on jpx-bug.pdf (which is exactly the pre-fix behaviour,
// ~53% of runs); add a second gate function to JpxSecurityGate.h without a row
// and part 3 fails; delete a fixture from the corpus directory and part 1's
// floor assertion fails.
//
// NOTE ON THE JPX DEFAULT. This test asserts the gate is OFF and does not
// change that. If it is ever found ON by default, that is a security
// regression to escalate, not to quietly correct — the failure message says so.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/JpxSecurityGate.h"
#include "core/Jbig2SecurityGate.h"
#include "core/MuPdfContext.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path corpusImagesDir() {
    return fs::path(TEST_FIXTURES_DIR) / "flaw_corpus" / "images";
}

class DecoderExposurePostureTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        // Record the incoming gate state so a sibling test that toggled it
        // cannot make this one report a false posture regression, and so this
        // test cannot leak state into the next one.
        jpxAtEntry_ = pdfe::core::isJpxDecodingEnabled();
        jbig2AtEntry_ = pdfe::core::isJbig2DecodingEnabled();
    }
    void TearDown() override {
        pdfe::core::setJpxDecodingEnabled(jpxAtEntry_);
        pdfe::core::setJbig2DecodingEnabled(jbig2AtEntry_);
    }

    // Build + drop the page-0 display list. Completing the call at all is the
    // no-crash oracle: a heap-corrupting decode aborts the process (0xC0000374)
    // rather than returning, so reaching the next statement IS the assertion.
    bool renderPageZero(pdfe::core::Document* doc) {
        void* dl = doc->createDisplayList(0);
        if (dl != nullptr) {
            doc->context()->execute([dl](fz_context* c) {
                fz_drop_display_list(c, static_cast<fz_display_list*>(dl));
            });
            return true;
        }
        return false;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    bool jpxAtEntry_ = false;
    bool jbig2AtEntry_ = false;
};

}  // namespace

// PART 2 — the shipping default posture, asserted before anything is touched.
TEST_F(DecoderExposurePostureTest, EveryDecodeGateDefaultsToSecure) {
    EXPECT_FALSE(jpxAtEntry_)
        << "JPEG2000 decoding is ENABLED by default. That re-exposes the "
           "OpenJPEG heap-buffer-overflow (#50, DCA 0011) — an RCE-class defect "
           "in bundled third-party code — to every PDF a user opens. This is a "
           "security regression to escalate, NOT to patch over by flipping the "
           "flag in a test.";

    EXPECT_FALSE(jbig2AtEntry_)
        << "JBIG2 decoding is ENABLED by default. That re-exposes jbig2dec — a "
           "decoder with a long history of memory-safety CVEs in its "
           "generic-region and symbol-dictionary paths (SEC-56 / #56) — to "
           "every PDF a user opens. Same rule as JPEG2000 above: escalate, do "
           "not flip the flag in a test.";
}

// PART 3 — the gate enumeration is grounded in the product's own header, so a
// new codec gate cannot be added without also being given a posture row above.
TEST_F(DecoderExposurePostureTest, DecodeGateEnumerationMatchesTheProduct) {
    // SEC-56: the enumeration is over every *SecurityGate.h in src/core, not a
    // single named file. When the JBIG2 gate arrived it went into a header of
    // its own, and a guard that only read JpxSecurityGate.h would have stayed
    // green while an entire second codec gate went unpinned — which is the
    // exact hole this test says it exists to close. Directory-driven, so gate
    // number three is caught without an edit here.
    const fs::path coreDir = fs::path(PDFE_REPO_ROOT) / "src" / "core";
    ASSERT_TRUE(fs::exists(coreDir))
        << "cannot find " << coreDir.string()
        << " — this guard must fail closed, not pass silently";

    std::vector<fs::path> gateHeaders;
    for (const auto& e : fs::directory_iterator(coreDir)) {
        const std::string fn = e.path().filename().string();
        if (e.is_regular_file() &&
            fn.find("SecurityGate.h") != std::string::npos) {
            gateHeaders.push_back(e.path());
        }
    }
    ASSERT_FALSE(gateHeaders.empty())
        << "no *SecurityGate.h headers found in " << coreDir.string()
        << " — an empty enumeration would make this guard vacuous";

    // Every `inline bool isXxxEnabled()` in a gate header is a decode gate
    // whose default this suite is responsible for pinning.
    const std::regex getterRx(R"(inline\s+bool\s+(is\w*Enabled)\s*\()");
    std::set<std::string> gates;
    for (const auto& gateHeader : gateHeaders) {
        std::ifstream in(gateHeader, std::ios::binary);
        ASSERT_TRUE(in.good()) << gateHeader.string();
        std::string line;
        while (std::getline(in, line)) {
            const auto firstNonSpace = line.find_first_not_of(" \t\r");
            if (firstNonSpace != std::string::npos &&
                line.compare(firstNonSpace, 2, "//") == 0) {
                continue;
            }
            std::smatch m;
            if (std::regex_search(line, m, getterRx)) gates.insert(m[1].str());
        }
    }

    const std::set<std::string> pinned{"isJpxDecodingEnabled",
                                       "isJbig2DecodingEnabled"};
    EXPECT_EQ(gates, pinned)
        << "the set of decode gates in src/core/*SecurityGate.h changed. Every "
           "gate needs a default-secure row in EveryDecodeGateDefaultsToSecure — "
           "a codec gate that ships on-by-default is DCA 0011 all over again.";
}

// PART 1 — the directory-driven corpus sweep. This is the row set that grows by
// itself.
TEST_F(DecoderExposurePostureTest, EveryImageCorpusFixtureSurvivesTheDefaultPosture) {
    const fs::path dir = corpusImagesDir();
    ASSERT_TRUE(fs::exists(dir))
        << "image flaw corpus missing at " << dir.string()
        << " — the sweep must fail closed, never pass on an empty directory";

    std::vector<fs::path> fixtures;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".pdf") {
            fixtures.push_back(e.path());
        }
    }
    std::sort(fixtures.begin(), fixtures.end());

    // A floor, not an exact count: the point of a directory-driven sweep is
    // that ADDING a fixture needs no edit here. Losing one, however, silently
    // shrinks the corpus — which is how coverage evaporates unnoticed.
    ASSERT_GE(fixtures.size(), 8u)
        << "the image flaw corpus has shrunk to " << fixtures.size()
        << " fixtures. Fixtures are only ever added; a drop means files were "
           "lost or the downloader did not run, and the sweep below would then "
           "pass while testing less than it did yesterday.";

    // The known heap-corrupting fixture must be among them — its absence would
    // make this sweep green for the wrong reason.
    const bool hasJpxCrasher =
        std::any_of(fixtures.begin(), fixtures.end(), [](const fs::path& p) {
            return p.filename().string() == "jpx-bug.pdf";
        });
    EXPECT_TRUE(hasJpxCrasher)
        << "images/jpx-bug.pdf (the #50 / DCA 0011 heap-overflow fixture) is "
           "not in the corpus — the sweep cannot demonstrate the default "
           "posture actually contains the known crasher";

    // The shipping default, asserted rather than assumed, and NOT modified.
    ASSERT_FALSE(pdfe::core::isJpxDecodingEnabled())
        << "refusing to run the hostile-fixture sweep with JPX decoding "
           "enabled: that is the configuration that corrupts the heap";

    for (const auto& f : fixtures) {
        const std::string name = f.filename().string();
        SCOPED_TRACE(name);

        // Reaching the statement after each call is the oracle. A decoder that
        // writes past a heap block takes the whole process down (0xC0000374);
        // there is no exception to catch and no value to inspect.
        std::unique_ptr<pdfe::core::Document> doc;
        ASSERT_NO_THROW({ doc = pdfe::core::Document::open(ctx_, f); })
            << name << " threw a C++ exception on open at the default decoder "
                       "posture; a malformed input must fail cleanly or not at all";
        if (!doc) {
            // A clean refusal to open is an acceptable outcome for a malformed
            // file — a crash is not, and we already survived it.
            continue;
        }
        ASSERT_GE(doc->pageCount(), 1) << name << " opened with no pages";
        EXPECT_NO_THROW({ renderPageZero(doc.get()); })
            << name << " threw while rendering page 0 at the default decoder "
                       "posture. At the shipping default no attacker-supplied "
                       "stream may reach a bundled codec that can corrupt the "
                       "heap (DCA 0011).";
        EXPECT_FALSE(doc->isModified())
            << name << ": merely opening and rendering must not mutate the "
                       "document";
    }
}
