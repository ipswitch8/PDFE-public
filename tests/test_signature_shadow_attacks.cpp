// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F21 — gaps PDF-36 and PDF-37.
//
// PDF-36: the incremental-update SHADOW ATTACK and SIGNATURE WRAPPING classes
// were untested. `grep -rniE 'shadow attack|signature wrapping' src/ tests/
// docs/` returned 0.
//
// PDF-37: 110 `ByteRange` hits in tests/ and not one of them asked whether the
// /ByteRange COVERS the file. They are all about MuPDF's byte-range BUG guard
// (`VerifyOutcome::ByteRangeBug`), which is a different question.
//
// The two gaps are one gap seen from two sides. `verifySignature()` asks "does
// the PKCS#7 digest over the byte ranges THE FILE STATES check out". The
// attacker chooses those ranges. So:
//
//   * append an incremental update after the signed range and the digest still
//     verifies while the rendered document says something else entirely;
//   * state a /ByteRange whose second range starts after an injected block,
//     re-sign over those ranges, and the digest verifies while 2 059 bytes in
//     the middle of the file are vouched for by nobody.
//
// Every assertion below is against `SignatureCoverageAnalyzer` — the product
// capability F21 added — and against AUTHORED ground truth read out of
// `tests/fixtures/sig_shadow_corpus_manifest.json`, never against whatever
// PDFE happens to report. The corpus generator is
// `tests/fixtures/gen_sig_shadow_corpus.py`; every fixture is synthetic and
// signed with the repository's own committed test key.
//
// The corpus also carries the ISO-layout certification fixtures and the
// attacker-added revision chain, which is why the DocMDP anchor-cap test lives
// here rather than in test_docmdp_enforcement.cpp: it needs the same fixtures.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "signatures/DocMdpEnforcer.h"
#include "signatures/SignatureCoverageAnalyzer.h"
#include "signatures/SignatureData.h"
#include "signatures/SignatureManager.h"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::signatures;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

/// The generator's own literals. Anchoring to this — not to PDFE's output —
/// is what makes these oracles able to fail.
const QJsonObject& manifest() {
    static QJsonObject cached = [] {
        QFile f(QString::fromStdString(
            (fixturesDir() / "sig_shadow_corpus_manifest.json").string()));
        if (!f.open(QIODevice::ReadOnly)) return QJsonObject{};
        return QJsonDocument::fromJson(f.readAll()).object();
    }();
    return cached;
}

QJsonObject facts(const char* fixture) {
    return manifest().value("fixtures").toObject()
               .value(QString::fromLatin1(fixture)).toObject();
}

std::vector<unsigned char> readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

DocumentCoverage coverageOf(const char* fixture) {
    auto ctx = std::make_shared<MuPdfContext>();
    SignatureCoverageAnalyzer analyzer(ctx);
    return analyzer.analyze(fixturesDir() / fixture);
}

std::string describe(const SignatureCoverage& c) {
    std::string br;
    for (int64_t v : c.byteRange) br += std::to_string(v) + " ";
    return std::string(coverageVerdictToString(c.verdict))
         + " | ByteRange [" + br + "] file=" + std::to_string(c.fileSize)
         + " signed=" + std::to_string(c.signedBytes)
         + " after=" + std::to_string(c.unsignedBytesAfterRange)
         + " gaps=" + std::to_string(c.unsignedBytesInGaps)
         + " | " + c.message;
}

std::string plainTextOf(const std::filesystem::path& p) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, p);
    if (!doc) return {};
    auto page = doc->page(0);
    return page ? page->extractPlainText() : std::string();
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

DocMdpStatus docMdpOf(const std::filesystem::path& p) {
    auto ctx = std::make_shared<MuPdfContext>();
    SignatureManager mgr(ctx);
    auto doc = Document::open(ctx, p);
    EXPECT_NE(doc, nullptr);
    return mgr.checkDocMdp(doc.get());
}

std::string describe(const DocMdpStatus& st) {
    std::string s = st.message
        + " [certified=" + (st.certified ? "yes" : "no")
        + " layout=" + docMdpLayoutToString(st.layout)
        + " level=" + std::to_string(static_cast<int>(st.level))
        + " certVersion=" + std::to_string(st.certificationVersion)
        + " totalVersions=" + std::to_string(st.totalVersions)
        + " changes=" + std::to_string(st.changes.size())
        + " locked=" + std::to_string(st.lockedFieldChanges.size()) + "]";
    for (const auto& c : st.changes) {
        s += "\n    obj " + std::to_string(c.objectNumber)
             + " v" + std::to_string(c.version)
             + " -> " + docMdpChangeClassToString(c.changeClass)
             + (c.touchesLockedField ? " [LOCKED FIELD]" : "");
    }
    return s;
}

} // namespace

// ===========================================================================
// Non-vacuity. Every test below reads the manifest; if it were missing or
// empty they would all pass by asserting nothing against nothing.
// ===========================================================================

TEST(SignatureShadowCorpus, ManifestAndFixturesArePresent) {
    ASSERT_FALSE(manifest().isEmpty())
        << "sig_shadow_corpus_manifest.json is missing or unparseable — "
           "regenerate with tests/fixtures/gen_sig_shadow_corpus.py";
    const QJsonObject all = manifest().value("fixtures").toObject();
    // 9 through F21; F26's security-audit remediation adds
    // sig_orphan_widget_signed.pdf (applied signature reachable only from the
    // page /Annots), making 10.
    EXPECT_EQ(all.size(), 10)
        << "the corpus is expected to carry 10 fixtures; the manifest lists "
        << all.size();
    ASSERT_TRUE(manifest().value("synthetic").toBool())
        << "every fixture must be synthetic — no live exploit against a third "
           "party may enter this corpus";

    for (const QString& name : all.keys()) {
        const auto p = fixturesDir() / name.toStdString();
        EXPECT_TRUE(std::filesystem::exists(p)) << "missing fixture " << name.toStdString();
        EXPECT_EQ(static_cast<int64_t>(std::filesystem::file_size(p)),
                  static_cast<int64_t>(all.value(name).toObject()
                                          .value("bytes").toDouble()))
            << name.toStdString()
            << " differs in size from the manifest — the fixture and its "
               "authored ground truth have drifted apart";
    }
}

// ===========================================================================
// PDF-37 — does /ByteRange cover the file?
//
// The register's proposed test name is used verbatim. The clean fixture is the
// OVER-REFUSAL CONTROL for everything below: if a real, untampered signature
// were reported as not covering the file, every "we detected it" verdict in
// this file would be noise.
// ===========================================================================

TEST(SignatureShadowAttacks, ByteRangeCoversEntireFileExceptContentsHole) {
    const auto cov = coverageOf("sig_clean_signed.pdf");
    ASSERT_TRUE(cov.readable);
    ASSERT_EQ(cov.signatureCount, 1);
    const auto& sc = cov.signatures.front();
    const QJsonObject f = facts("sig_clean_signed.pdf");

    EXPECT_EQ(sc.verdict, CoverageVerdict::CoversWholeFile) << describe(sc);
    EXPECT_TRUE(cov.allSignaturesCoverWholeFile);

    // The authored figures, not PDFE's.
    EXPECT_EQ(sc.fileSize, static_cast<int64_t>(f.value("bytes").toDouble()));
    EXPECT_EQ(sc.signedBytes,
              static_cast<int64_t>(f.value("signed_bytes").toDouble()));
    EXPECT_EQ(sc.unsignedBytesAfterRange, 0);
    EXPECT_EQ(sc.unsignedBytesInGaps, 0);
    EXPECT_EQ(sc.byteRangeArraysInFile, 1);

    // The arithmetic that "covers the whole file except the /Contents hole"
    // actually means, asserted rather than trusted.
    ASSERT_GT(sc.contentsHoleStart, 0);
    ASSERT_GT(sc.contentsHoleEnd, sc.contentsHoleStart);
    EXPECT_EQ(sc.signedBytes + (sc.contentsHoleEnd - sc.contentsHoleStart),
              sc.fileSize)
        << "signed bytes plus the /Contents hole must be the whole file";

    // And the hole really is the hex string, at the offsets reported.
    const auto bytes = readAll(fixturesDir() / "sig_clean_signed.pdf");
    ASSERT_LT(static_cast<size_t>(sc.contentsHoleEnd), bytes.size() + 1);
    EXPECT_EQ(bytes[static_cast<size_t>(sc.contentsHoleStart)], '<');
    EXPECT_EQ(bytes[static_cast<size_t>(sc.contentsHoleEnd - 1)], '>');
}

// A file whose /ByteRange is not four integers must be rejected outright. The
// tamper is applied HERE, to a copy, so the corpus does not have to carry a
// fixture whose only purpose is to be malformed.
TEST(SignatureShadowAttacks, MalformedByteRangeIsRejectedNotSilentlyAccepted) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto dst = std::filesystem::path(tmp.path().toStdString())
                   / "malformed_byterange.pdf";

    auto bytes = readAll(fixturesDir() / "sig_clean_signed.pdf");
    const std::string needle = "/ByteRange [0 ";
    auto it = std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end());
    ASSERT_NE(it, bytes.end());
    const size_t at = static_cast<size_t>(it - bytes.begin());
    const size_t close = std::find(bytes.begin() + at, bytes.end(), ']')
                       - bytes.begin();
    // Six entries, same total width, so no offset in the file moves.
    const std::string replacement = "/ByteRange [0 1 2 3 4 5";
    ASSERT_LE(replacement.size(), close - at);
    for (size_t i = 0; i < close - at; ++i) {
        bytes[at + i] = (i < replacement.size())
                            ? static_cast<unsigned char>(replacement[i])
                            : static_cast<unsigned char>(' ');
    }
    std::ofstream(dst, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));

    auto ctx = std::make_shared<MuPdfContext>();
    SignatureCoverageAnalyzer analyzer(ctx);
    const auto cov = analyzer.analyze(dst);
    ASSERT_EQ(cov.signatureCount, 1);
    EXPECT_EQ(cov.signatures.front().verdict, CoverageVerdict::Malformed)
        << describe(cov.signatures.front());
    EXPECT_FALSE(cov.allSignaturesCoverWholeFile);
}

// ===========================================================================
// PDF-36 (a) — the incremental-update shadow attack.
//
// The specific detection asserted is `ContentAppendedAfterSignedRange` with
// the EXACT authored byte count. "It didn't crash" would not be a detection,
// and neither would "isFullyValid() is false" — the test certificate is
// self-signed, so that would be false for the clean control too and would
// discriminate nothing.
// ===========================================================================

class ShadowAttackFixture : public ::testing::TestWithParam<const char*> {};

TEST_P(ShadowAttackFixture, AppendedRevisionIsDetectedAsUnsignedContent) {
    const char* name = GetParam();
    const QJsonObject f = facts(name);
    ASSERT_FALSE(f.isEmpty()) << name;

    const auto cov = coverageOf(name);
    ASSERT_TRUE(cov.readable);
    ASSERT_EQ(cov.signatureCount, 1);
    const auto& sc = cov.signatures.front();

    EXPECT_EQ(sc.verdict, CoverageVerdict::ContentAppendedAfterSignedRange)
        << name << ": " << describe(sc);
    EXPECT_FALSE(cov.allSignaturesCoverWholeFile);
    EXPECT_EQ(sc.unsignedBytesAfterRange,
              static_cast<int64_t>(f.value("appended_bytes").toDouble()))
        << name << ": the number of unsigned appended bytes must match the "
                   "generator's own figure exactly. " << describe(sc);
    EXPECT_EQ(sc.unsignedBytesInGaps, 0) << name;
}

// The signed range is BYTE-IDENTICAL to the clean fixture. This is what makes
// it a shadow attack rather than a forgery: nothing the signature covers has
// changed, so no digest check can see it.
TEST_P(ShadowAttackFixture, SignedRangeIsByteIdenticalToTheCleanSignedFile) {
    const char* name = GetParam();
    const auto clean = readAll(fixturesDir() / "sig_clean_signed.pdf");
    const auto attacked = readAll(fixturesDir() / name);
    ASSERT_FALSE(clean.empty());
    ASSERT_GT(attacked.size(), clean.size());

    EXPECT_TRUE(std::equal(clean.begin(), clean.end(), attacked.begin()))
        << name << ": the first " << clean.size()
        << " bytes must be the clean signed file, unmodified";
    EXPECT_EQ(attacked.size() - clean.size(),
              static_cast<size_t>(facts(name).value("appended_bytes").toDouble()));
}

// ...and the RENDERED document is different. Without this the corpus would
// only prove that bytes were appended, not that the appended bytes change what
// a human reads — which is the entire threat.
TEST_P(ShadowAttackFixture, RenderedTextDiffersFromTheSignedRevision) {
    const char* name = GetParam();
    const QJsonObject f = facts(name);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto signedOnly = std::filesystem::path(tmp.path().toStdString())
                          / "signed_revision_only.pdf";
    // The signed revision, recovered by keeping only the bytes the signature
    // vouches for — i.e. exactly what a verifier's digest attests to.
    const auto cov = coverageOf(name);
    ASSERT_EQ(cov.signatureCount, 1);
    const auto& sc = cov.signatures.front();
    const int64_t signedEnd = sc.byteRange[2] + sc.byteRange[3];
    const auto bytes = readAll(fixturesDir() / name);
    ASSERT_GT(signedEnd, 0);
    std::ofstream(signedOnly, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(signedEnd));

    const std::string signedText = plainTextOf(signedOnly);
    const std::string renderedText = plainTextOf(fixturesDir() / name);

    ASSERT_FALSE(signedText.empty()) << name << ": signed revision has no text";
    ASSERT_FALSE(renderedText.empty()) << name << ": rendered file has no text";

    const std::string original =
        f.value("original_line").toString().toStdString();
    const std::string rendered =
        f.value("rendered_line").toString().toStdString();

    EXPECT_TRUE(contains(signedText, original))
        << name << ": the SIGNED revision must still say \"" << original
        << "\"; it says: " << signedText;
    EXPECT_FALSE(contains(renderedText, original))
        << name << ": the rendered document must no longer show the signed "
                   "wording; it says: " << renderedText;
    EXPECT_TRUE(contains(renderedText, rendered))
        << name << ": the rendered document must show \"" << rendered
        << "\"; it says: " << renderedText;
    EXPECT_NE(signedText, renderedText)
        << name << ": signature valid, and the document reads differently";
}

INSTANTIATE_TEST_SUITE_P(ShadowAttacks, ShadowAttackFixture,
                         ::testing::Values("sig_shadow_hide.pdf",
                                           "sig_shadow_replace.pdf"),
                         [](const auto& info) {
                             std::string n = info.param;
                             n = n.substr(0, n.find('.'));
                             std::replace(n.begin(), n.end(), '_', 'X');
                             return n;
                         });

// ===========================================================================
// PDF-36 (b) — signature wrapping.
//
// A block of bytes sits INSIDE the file and outside every signed range, and
// the signature was re-signed over the manipulated ranges so the digest is
// genuinely valid. The only thing wrong with this file is its coverage, which
// is precisely the property nothing checked before F21.
// ===========================================================================

TEST(SignatureShadowAttacks, SignatureWrappingLeavesAnUnsignedGapInsideTheFile) {
    const QJsonObject f = facts("sig_wrapping_two_byteranges.pdf");
    ASSERT_FALSE(f.isEmpty());

    const auto cov = coverageOf("sig_wrapping_two_byteranges.pdf");
    ASSERT_TRUE(cov.readable);
    ASSERT_EQ(cov.signatureCount, 1);
    const auto& sc = cov.signatures.front();

    EXPECT_EQ(sc.verdict, CoverageVerdict::GapInSignedRange) << describe(sc);
    EXPECT_FALSE(cov.allSignaturesCoverWholeFile);
    EXPECT_EQ(sc.unsignedBytesInGaps,
              static_cast<int64_t>(f.value("gap_bytes").toDouble()))
        << "the gap must be exactly the authored size. " << describe(sc);
    // Nothing was appended: the attack is entirely inside the file, which is
    // what distinguishes wrapping from the shadow attacks above.
    EXPECT_EQ(sc.unsignedBytesAfterRange, 0) << describe(sc);

    // The file carries a second /ByteRange array that no live signature owns.
    EXPECT_EQ(sc.byteRangeArraysInFile,
              f.value("byte_range_arrays_in_file").toInt())
        << "the decoy /ByteRange must be visible in the raw bytes";
    EXPECT_GT(sc.byteRangeArraysInFile, cov.signatureCount)
        << "more /ByteRange arrays than signatures is the wrapping tell";

    // And the injected payload really does live in the unsigned gap.
    const auto bytes = readAll(fixturesDir() / "sig_wrapping_two_byteranges.pdf");
    const std::string marker = f.value("gap_marker").toString().toStdString();
    ASSERT_FALSE(marker.empty());
    auto it = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
    ASSERT_NE(it, bytes.end()) << "injected marker not present in the file";
    const int64_t markerAt = static_cast<int64_t>(it - bytes.begin());
    EXPECT_GE(markerAt, sc.contentsHoleEnd)
        << "the marker must start after the /Contents hole";
    EXPECT_LT(markerAt + static_cast<int64_t>(marker.size()), sc.byteRange[2])
        << "the marker must end before the second signed range begins — i.e. "
           "lie wholly inside the unsigned gap";
}

// ===========================================================================
// The measurement that makes the whole gap matter, stated as it MEASURES —
// not as it would be convenient to state it.
//
// For the two incremental-update shadow attacks the PKCS#7 digest still
// verifies. That is the finding: digest-only verification cannot see them.
//
// For the WRAPPING fixture it does not, and the reason is worth recording
// rather than hiding: MuPDF derives the extent of the /Contents hex string
// from /ByteRange itself (offset r0+r1 up to r2). Widening r2 to skip the
// injected block therefore makes MuPDF try to hex-decode the injected bytes,
// and it refuses with "signature certificate data contains invalid
// character". So this shape of wrapping is rejected by the engine — loudly,
// as a parse failure, with no statement about coverage. That is a real
// containment, and it is NOT the same as reporting the coverage defect, which
// is what SignatureCoverageAnalyzer adds.
// ===========================================================================

namespace {

struct VerifyOutcome {
    SignatureVerifyResult result;
    std::string thrown;
};

VerifyOutcome verifyFirstSignature(const char* name) {
    VerifyOutcome out;
    auto ctx = std::make_shared<MuPdfContext>();
    SignatureManager mgr(ctx);
    auto doc = Document::open(ctx, fixturesDir() / name);
    if (!doc) { out.thrown = "could not open"; return out; }
    auto page = doc->page(0);
    if (!page) { out.thrown = "no page 0"; return out; }
    try {
        out.result = mgr.verifySignature(doc.get(), page.get(), 0);
    } catch (const std::exception& e) {
        out.thrown = e.what();
    }
    return out;
}

} // namespace

TEST(SignatureShadowAttacks, DigestOnlyVerificationCannotSeeTheShadowAttacks) {
    for (const char* name : {"sig_clean_signed.pdf",
                             "sig_shadow_hide.pdf",
                             "sig_shadow_replace.pdf"}) {
        const auto v = verifyFirstSignature(name);
        EXPECT_TRUE(v.thrown.empty()) << name << ": verify threw: " << v.thrown;
        EXPECT_TRUE(v.result.digestValid)
            << name << ": the digest must remain VALID — that is what makes "
                       "an appended-revision shadow attack invisible to "
                       "digest-only verification. " << v.result.digestMessage;
    }

    // The clean control and the two attacks are indistinguishable on the
    // digest, and distinguishable on coverage. Both halves asserted, because
    // either alone would be a weaker claim than the one being made.
    EXPECT_TRUE(coverageOf("sig_clean_signed.pdf").allSignaturesCoverWholeFile);
    EXPECT_FALSE(coverageOf("sig_shadow_hide.pdf").allSignaturesCoverWholeFile);
    EXPECT_FALSE(coverageOf("sig_shadow_replace.pdf").allSignaturesCoverWholeFile);
}

TEST(SignatureShadowAttacks, WrappedSignatureIsRefusedByTheEngineToo) {
    const auto v = verifyFirstSignature("sig_wrapping_two_byteranges.pdf");
    EXPECT_FALSE(v.result.digestValid)
        << "a /ByteRange that skips injected bytes must not yield a valid "
           "digest verdict";
    EXPECT_FALSE(v.thrown.empty())
        << "MuPDF derives the /Contents extent from /ByteRange, so widening "
           "the second range makes it try to hex-decode the injected block "
           "and refuse. If this stops throwing, the engine has started "
           "accepting the wrapped container and the coverage verdict below "
           "becomes the ONLY line of defence — which is worth knowing.";
    // The coverage verdict is the specific, structural detection, and it does
    // not depend on the engine happening to choke.
    EXPECT_FALSE(
        coverageOf("sig_wrapping_two_byteranges.pdf").allSignaturesCoverWholeFile);
}

// ===========================================================================
// ISO-layout certification.
//
// PDFE's own writer puts /TransformParams directly on /Perms /DocMDP. Acrobat
// and pyHanko make /Perms /DocMDP the SIGNATURE dictionary and put /P inside
// /Reference [ /SigRef ]. Reading only the first layout meant a third-party P1
// document fell through to the /P default of 2 — under-enforcement, silently.
// The P1/P3 pair discriminates: a reader that ignores /Reference reports 2 for
// both.
// ===========================================================================

TEST(SignatureShadowAttacks, IsoLayoutCertificationLevelIsReadFromSigRef) {
    const auto p1 = docMdpOf(fixturesDir() / "sig_iso_docmdp_p1.pdf");
    const auto p3 = docMdpOf(fixturesDir() / "sig_iso_docmdp_p3.pdf");

    ASSERT_TRUE(p1.certified) << describe(p1);
    ASSERT_TRUE(p3.certified) << describe(p3);
    EXPECT_EQ(p1.layout, DocMdpLayout::IsoSigRefReference) << describe(p1);
    EXPECT_EQ(p3.layout, DocMdpLayout::IsoSigRefReference) << describe(p3);

    EXPECT_EQ(p1.level, CertificationLevel::NoChanges)
        << "a /P 1 document written in the ISO layout must be read as P1, not "
           "as the /P default of 2. " << describe(p1);
    EXPECT_EQ(p3.level, CertificationLevel::AnnotationsAllowed)
        << "and the same reader must report P3 for the P3 fixture — otherwise "
           "it is returning a constant. " << describe(p3);

    // Untouched: neither may be reported broken.
    EXPECT_FALSE(p1.violated) << describe(p1);
    EXPECT_FALSE(p3.violated) << describe(p3);
}

// ===========================================================================
// The anchor-cap gap the F2 security-audit gate asked for.
//
// DocMdpEnforcer advances its certification anchor from the signature's
// revision to the catalog's, because MuPDF's signing save writes the /Perms
// stamp in a separate xref section. That advance is capped at ONE section. If
// it were not, an attacker could append revisions that rewrite the catalog and
// walk the anchor forward to the newest revision, hiding everything before it.
//
// This fixture appends THREE such revisions. Until now the cap was
// desk-checked; this is the test.
// ===========================================================================

TEST(SignatureShadowAttacks, DocMdpAnchorSurvivesAttackerAddedRevisionChain) {
    const QJsonObject f = facts("sig_docmdp_multirev.pdf");
    ASSERT_FALSE(f.isEmpty());
    const int addedRevisions = f.value("attacker_revisions").toInt();
    ASSERT_EQ(addedRevisions, 3);

    const auto baseline = docMdpOf(fixturesDir() / "sig_iso_docmdp_p1.pdf");
    const auto attacked = docMdpOf(fixturesDir() / "sig_docmdp_multirev.pdf");

    // Same document, same certification, three extra revisions on top.
    ASSERT_TRUE(attacked.certified) << describe(attacked);
    EXPECT_EQ(attacked.level, CertificationLevel::NoChanges) << describe(attacked);
    EXPECT_EQ(attacked.totalVersions, baseline.totalVersions + addedRevisions)
        << "the fixture must really carry " << addedRevisions
        << " more revisions than the clean certified file. "
        << describe(attacked);

    // THE ANCHOR DID NOT MOVE. Expressed as "how many revisions newer than the
    // anchor exist", which is what an attacker is trying to shrink to zero.
    EXPECT_EQ(attacked.certificationVersion, addedRevisions)
        << "the anchor must still sit at the certifying revision, i.e. "
        << addedRevisions << " versions back from the newest. If this reports "
           "0 the anchor walked forward with the attacker's catalog rewrites "
           "and every change below became invisible. " << describe(attacked);

    // ...and every attacker revision is still attributed.
    EXPECT_TRUE(attacked.changedSinceCertification) << describe(attacked);
    EXPECT_TRUE(attacked.violated)
        << "a /P 1 document permits nothing at all. " << describe(attacked);

    EXPECT_GE(static_cast<int>(attacked.changes.size()), addedRevisions)
        << "at least one object per attacker revision must be attributed. "
        << describe(attacked);

    // The catalog was rewritten in every one of the attacker's revisions, so
    // the change list must span all three version numbers 0..addedRevisions-1.
    for (int v = 0; v < addedRevisions; ++v) {
        const bool seen = std::any_of(
            attacked.changes.begin(), attacked.changes.end(),
            [v](const DocMdpChange& c) { return c.version == v; });
        EXPECT_TRUE(seen)
            << "no change attributed to attacker revision at version " << v
            << " — an anchor that had advanced would swallow exactly these. "
            << describe(attacked);
    }

    // And the appended bytes are outside the signed range, which is how the
    // attacker got them in without touching the digest.
    const auto cov = coverageOf("sig_docmdp_multirev.pdf");
    ASSERT_EQ(cov.signatureCount, 1);
    EXPECT_EQ(cov.signatures.front().verdict,
              CoverageVerdict::ContentAppendedAfterSignedRange)
        << describe(cov.signatures.front());
    EXPECT_EQ(cov.signatures.front().unsignedBytesAfterRange,
              static_cast<int64_t>(f.value("unsigned_after_range").toDouble()));
}
