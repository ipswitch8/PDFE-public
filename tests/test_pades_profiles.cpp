// -*- coding: utf-8 -*-
// tests/test_pades_profiles.cpp
//
// Meta-audit follow-up F20 — gap ids PDF-34 (Critical) and PDF-35 (High).
//
// The finding: PDFE emitted `adbe.pkcs7.detached` and never
// `ETSI.CAdES.detached`, so it was NOT PAdES — while five source comments, the
// public README and three capability documents said it was PAdES-B-LT. There
// is no document-level /DocTimeStamp anywhere in src/, which makes B-LTA
// structurally impossible rather than merely untested.
//
// The disposition (see docs/audit/meta-audit-2026-07/followup/
// phase-20-dispositions.md) is to DISCLAIM rather than implement: emitting the
// ETSI SubFilter over a container that fails two mandatory CAdES rules would be
// a worse false claim than the one being removed, and there is no conformance
// oracle in this tree that could substantiate a profile claim.
//
// A disclaimer is only worth something if it cannot drift, so this file does
// two things:
//
//   1. MEASURES what the product emits — the /SubFilter name in the saved
//      bytes, the CMS signed-attribute set of the PKCS#7 container, and the
//      absence of any document timestamp. These are behavioural oracles over a
//      really-signed file; they go red if the emitted bytes change.
//
//   2. TIES docs/signature-profile-support.md TO THOSE MEASUREMENTS, in both
//      directions. The document carries machine-readable declarations; the
//      guard re-derives each one from the tree and fails if the document and
//      the product disagree either way. Editing the document to claim PAdES
//      fails. Implementing PAdES without updating the document also fails. The
//      guard therefore cannot outlive the gap.
//
// Every oracle here was mutation-tested; the mutants and their verdicts are
// tabulated in the phase-20 dispositions file.

#include <gtest/gtest.h>

#include "signatures/SignatureManager.h"
#include "signatures/SignatureData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <openssl/asn1.h>
#include <openssl/objects.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using pdfe::signatures::SignatureManager;
using pdfe::signatures::SigningOptions;

namespace {

fs::path repoRoot() { return fs::path(PDFE_REPO_ROOT); }

std::string readFileBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// Raw-byte helpers. These deliberately read the SAVED FILE rather than asking
// MuPDF, because the claim under test is about what lands on disk for a relying
// party to parse — not about what our own model thinks it wrote.
// ---------------------------------------------------------------------------

/// Every `/SubFilter /Name` value present in @p bytes, in file order.
std::vector<std::string> subFilterNames(const std::string& bytes) {
    std::vector<std::string> out;
    const std::string key = "/SubFilter";
    size_t pos = 0;
    while ((pos = bytes.find(key, pos)) != std::string::npos) {
        size_t i = pos + key.size();
        while (i < bytes.size() && (bytes[i] == ' ' || bytes[i] == '\r' ||
                                    bytes[i] == '\n' || bytes[i] == '\t'))
            ++i;
        if (i < bytes.size() && bytes[i] == '/') {
            ++i;
            std::string name;
            while (i < bytes.size() &&
                   !std::strchr(" \t\r\n/[]<>()", bytes[i]))
                name += bytes[i++];
            if (!name.empty()) out.push_back(name);
        }
        pos += key.size();
    }
    return out;
}

/// True iff @p bytes carry a document-level timestamp signature — either the
/// `/Type /DocTimeStamp` marker or the `/ETSI.RFC3161` SubFilter that only a
/// document timestamp uses.
bool containsDocumentTimestamp(const std::string& bytes) {
    if (bytes.find("/DocTimeStamp") != std::string::npos) return true;
    const auto names = subFilterNames(bytes);
    return std::find(names.begin(), names.end(), "ETSI.RFC3161") != names.end();
}

/// Hex-decode the first `/Contents <hex...>` string in @p bytes. That syntax is
/// specific to a signature value dictionary (page /Contents is an indirect
/// reference, never a hex string), so no disambiguation is needed. Returns an
/// empty vector when there is none.
std::vector<unsigned char> signatureContentsDer(const std::string& bytes) {
    const std::string key = "/Contents";
    size_t pos = 0;
    while ((pos = bytes.find(key, pos)) != std::string::npos) {
        size_t i = pos + key.size();
        while (i < bytes.size() && (bytes[i] == ' ' || bytes[i] == '\r' ||
                                    bytes[i] == '\n' || bytes[i] == '\t'))
            ++i;
        if (i >= bytes.size() || bytes[i] != '<') { pos += key.size(); continue; }
        ++i;
        std::vector<unsigned char> der;
        int hi = -1;
        for (; i < bytes.size() && bytes[i] != '>'; ++i) {
            const char c = bytes[i];
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else continue;
            if (hi < 0) hi = v;
            else { der.push_back(static_cast<unsigned char>((hi << 4) | v)); hi = -1; }
        }
        return der;
    }
    return {};
}

/// The set of CMS **signed** attribute OIDs (dotted form) on the first
/// SignerInfo of the PKCS#7 blob in @p der. Empty when the blob will not parse
/// or carries no SignerInfo — callers must assert non-emptiness so an absence
/// assertion cannot pass because nothing was read.
std::set<std::string> signedAttributeOids(const std::vector<unsigned char>& der) {
    std::set<std::string> oids;
    if (der.empty()) return oids;
    const unsigned char* p = der.data();
    PKCS7* p7 = d2i_PKCS7(nullptr, &p, static_cast<long>(der.size()));
    if (!p7) return oids;

    STACK_OF(PKCS7_SIGNER_INFO)* sis = PKCS7_get_signer_info(p7);
    if (sis && sk_PKCS7_SIGNER_INFO_num(sis) > 0) {
        PKCS7_SIGNER_INFO* si = sk_PKCS7_SIGNER_INFO_value(sis, 0);
        if (si && si->auth_attr) {
            for (int i = 0; i < sk_X509_ATTRIBUTE_num(si->auth_attr); ++i) {
                X509_ATTRIBUTE* a = sk_X509_ATTRIBUTE_value(si->auth_attr, i);
                if (!a) continue;
                ASN1_OBJECT* obj = X509_ATTRIBUTE_get0_object(a);
                if (!obj) continue;
                char buf[128] = {0};
                if (OBJ_obj2txt(buf, sizeof(buf), obj, 1) > 0) oids.insert(buf);
            }
        }
    }
    PKCS7_free(p7);
    return oids;
}

// OIDs named rather than derived, so a rename in OpenSSL's NID table cannot
// silently turn an assertion vacuous.
constexpr const char* kOidContentType          = "1.2.840.113549.1.9.3";
constexpr const char* kOidMessageDigest        = "1.2.840.113549.1.9.4";
constexpr const char* kOidSigningTime          = "1.2.840.113549.1.9.5";
constexpr const char* kOidSigningCertificate   = "1.2.840.113549.1.9.16.2.12";  // ESS v1
constexpr const char* kOidSigningCertificateV2 = "1.2.840.113549.1.9.16.2.47";  // ESS v2 (RFC 5035)

// ---------------------------------------------------------------------------
// Fixture: produce one really-signed PDF and cache the facts read out of it.
// Signing is the expensive part; every measurement below is a read of the same
// bytes.
// ---------------------------------------------------------------------------

struct SignedFacts {
    std::string bytes;
    std::string subFilter;                 // the single SubFilter emitted
    std::set<std::string> signedAttrOids;
    bool hasDocumentTimestamp = false;
};

class PadesProfileTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixtureBase_ = fs::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        pfxPath_ = fs::path(TEST_FIXTURES_DIR) / "test_signer.pfx";
        ASSERT_TRUE(fs::exists(fixtureBase_)) << fixtureBase_.string();
        ASSERT_TRUE(fs::exists(pfxPath_))
            << "test_signer.pfx fixture missing — required for signing tests";
        manager_ = std::make_unique<SignatureManager>(ctx_);

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = fs::temp_directory_path() / "pdfe_pades_profiles" / info->name();
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
        fs::create_directories(tempDir_);
    }

    void TearDown() override {
        manager_.reset();
        ctx_.reset();
    }

    /// Create a signature field, sign it with the test PFX, and read the facts
    /// back out of the saved bytes.
    SignedFacts signAndMeasure() {
        const auto out = tempDir_ / "signed.pdf";
        {
            auto doc = pdfe::core::Document::open(ctx_, fixtureBase_);
            auto page = doc->page(0);
            manager_->createSignatureField(page.get(), "FieldA",
                                           QRectF(100, 500, 200, 50));
            doc->saveAs(out);
        }
        {
            auto doc = pdfe::core::Document::open(ctx_, out);
            auto page = doc->page(0);
            SigningOptions opts;
            opts.pfxPath = pfxPath_.string();
            opts.pfxPassword = "test123";
            opts.reason = "F20 profile measurement";
            opts.location = "Test Lab";
            manager_->signField(doc.get(), page.get(), 0, opts);
        }

        SignedFacts f;
        f.bytes = readFileBytes(out);
        const auto names = subFilterNames(f.bytes);
        if (names.size() == 1) f.subFilter = names.front();
        else if (!names.empty()) f.subFilter = names.back();
        f.signedAttrOids = signedAttributeOids(signatureContentsDer(f.bytes));
        f.hasDocumentTimestamp = containsDocumentTimestamp(f.bytes);
        return f;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    fs::path fixtureBase_;
    fs::path pfxPath_;
    fs::path tempDir_;
    std::unique_ptr<SignatureManager> manager_;
};

// ===========================================================================
// PDF-34 — what SubFilter actually lands in the file
// ===========================================================================

TEST_F(PadesProfileTest, EmittedSubFilterIsAdbePkcs7DetachedNotEtsiCades) {
    const auto f = signAndMeasure();
    ASSERT_FALSE(f.bytes.empty()) << "signed output is empty";

    const auto names = subFilterNames(f.bytes);
    ASSERT_EQ(names.size(), 1u)
        << "expected exactly one /SubFilter in a singly-signed file; got "
        << names.size();

    EXPECT_EQ(names.front(), "adbe.pkcs7.detached")
        << "PDFE's signature SubFilter changed. If PAdES was implemented, "
           "docs/signature-profile-support.md and "
           "docs/audit/meta-audit-2026-07/followup/phase-20-dispositions.md "
           "must change with it — see PDF-34.";

    EXPECT_EQ(f.bytes.find("ETSI.CAdES.detached"), std::string::npos)
        << "the saved bytes announce ETSI.CAdES.detached. PDFE's container "
           "does not satisfy CAdES-BES (no signing-certificate-v2, and a "
           "prohibited signing-time attribute), so announcing it would be a "
           "false conformance claim.";
}

// ===========================================================================
// PDF-34 — WHY it is not CAdES, measured rather than argued.
//
// Two independent conformance failures, in opposite directions. Both are read
// off the real container; the contentType/messageDigest assertions exist so
// that an "attribute is absent" verdict cannot come from an empty parse.
// ===========================================================================

TEST_F(PadesProfileTest, SignedContainerFailsCadesBesAttributeRules) {
    const auto f = signAndMeasure();
    const auto& oids = f.signedAttrOids;

    // Non-vacuity: the enumeration really read a SignerInfo's signed attrs.
    ASSERT_FALSE(oids.empty())
        << "no signed attributes were read from the PKCS#7 container — the "
           "absence assertions below would be meaningless";
    ASSERT_TRUE(oids.count(kOidContentType))
        << "content-type attribute missing; the attribute enumeration is wrong";
    ASSERT_TRUE(oids.count(kOidMessageDigest))
        << "message-digest attribute missing; the attribute enumeration is wrong";

    // Failure 1 — mandatory attribute absent.
    EXPECT_EQ(oids.count(kOidSigningCertificateV2), 0u)
        << "ESS signing-certificate-v2 is now present. That is a CAdES-BES "
           "prerequisite: if PAdES is being implemented, update "
           "docs/signature-profile-support.md (PDF-34).";
    EXPECT_EQ(oids.count(kOidSigningCertificate), 0u)
        << "ESS signing-certificate (v1) is now present — same note as above.";

    // Failure 2 — prohibited attribute present.
    EXPECT_EQ(oids.count(kOidSigningTime), 1u)
        << "the PKCS#9 signing-time attribute is no longer emitted. PAdES "
           "forbids it (the /M entry carries the claimed time), so its removal "
           "is a step toward conformance and must be reflected in "
           "docs/signature-profile-support.md (PDF-34).";
}

// ===========================================================================
// PDF-35 — no document-level timestamp is produced, so B-LTA is unreachable.
//
// The positive control is a literal byte string, not a fixture, so the detector
// is proved to work without depending on a generated file.
// ===========================================================================

TEST_F(PadesProfileTest, SigningProducesNoDocumentLevelTimestamp) {
    // Control: the detector fires on a document that DOES carry one.
    const std::string control =
        "%PDF-1.7\n1 0 obj\n<< /Type /DocTimeStamp /Filter /Adobe.PPKLite "
        "/SubFilter /ETSI.RFC3161 /Contents <30> >>\nendobj\n";
    ASSERT_TRUE(containsDocumentTimestamp(control))
        << "the document-timestamp detector does not detect a document "
           "timestamp; every negative below would be vacuous";
    // ...and does not fire on a document that does not.
    ASSERT_FALSE(containsDocumentTimestamp(
        "%PDF-1.7\n1 0 obj\n<< /Type /Sig /SubFilter /adbe.pkcs7.detached >>\n"))
        << "the detector fires on an ordinary approval signature";

    const auto f = signAndMeasure();
    EXPECT_FALSE(f.hasDocumentTimestamp)
        << "a document-level /DocTimeStamp appeared in the saved bytes. PDFE "
           "has no document-timestamp writer (PDF-35); if one was added, "
           "docs/signature-profile-support.md must stop recording "
           "'document-timestamp: not-implemented' and B-LTA scope must be "
           "re-assessed.";
}

// ===========================================================================
// Document guards — the disclaimer must track the product in BOTH directions.
// ===========================================================================

/// Parse `key: value` lines out of the machine-readable block of
/// docs/signature-profile-support.md.
std::map<std::string, std::string> profileDocDeclarations() {
    std::map<std::string, std::string> out;
    const std::string body =
        readFileBytes(repoRoot() / "docs" / "signature-profile-support.md");
    const size_t b = body.find("<!-- BEGIN MACHINE-READABLE");
    const size_t e = body.find("<!-- END MACHINE-READABLE");
    if (b == std::string::npos || e == std::string::npos || e <= b) return out;
    std::istringstream in(body.substr(b, e - b));
    std::string line;
    const std::regex kv(R"(^\s*([a-z0-9\-]+):\s*(\S+)\s*$)");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_match(line, m, kv)) out[m[1].str()] = m[2].str();
    }
    return out;
}

std::vector<std::string> capabilityDocs() {
    std::vector<std::string> out;
    const std::string body =
        readFileBytes(repoRoot() / "docs" / "signature-profile-support.md");
    const size_t b = body.find("<!-- BEGIN CAPABILITY-DOCS");
    const size_t e = body.find("<!-- END CAPABILITY-DOCS");
    if (b == std::string::npos || e == std::string::npos || e <= b) return out;
    std::istringstream in(body.substr(b, e - b));
    std::string line;
    const std::regex kv(R"(^\s*capability-doc:\s*(\S+)\s*$)");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_match(line, m, kv)) out.push_back(m[1].str());
    }
    return out;
}

TEST_F(PadesProfileTest, ProfileDocDeclarationsMatchMeasuredBehaviour) {
    const auto decl = profileDocDeclarations();
    ASSERT_FALSE(decl.empty())
        << "docs/signature-profile-support.md has no machine-readable block; "
           "the single-source-of-truth document required by F20 is gone or "
           "unparseable";
    for (const char* key : {"emitted-subfilter",
                            "cades-signing-certificate-v2-attribute",
                            "cms-signing-time-attribute",
                            "pades-b-b", "pades-b-t", "pades-b-lt",
                            "pades-b-lta", "document-timestamp"}) {
        ASSERT_EQ(decl.count(key), 1u)
            << "docs/signature-profile-support.md is missing the declaration '"
            << key << "'";
    }

    const auto f = signAndMeasure();

    // --- 1. Declared SubFilter must equal the emitted SubFilter. ---------
    EXPECT_EQ(decl.at("emitted-subfilter"), f.subFilter)
        << "docs/signature-profile-support.md declares emitted-subfilter '"
        << decl.at("emitted-subfilter") << "' but the product emitted '"
        << f.subFilter << "'";

    // --- 2. Declared CMS attribute facts must equal the measured ones. ---
    ASSERT_FALSE(f.signedAttrOids.empty()) << "no signed attributes read";
    const bool essPresent = f.signedAttrOids.count(kOidSigningCertificateV2) > 0;
    const bool stPresent = f.signedAttrOids.count(kOidSigningTime) > 0;
    EXPECT_EQ(decl.at("cades-signing-certificate-v2-attribute"),
              essPresent ? "present" : "absent")
        << "the document's signing-certificate-v2 declaration disagrees with "
           "the container";
    EXPECT_EQ(decl.at("cms-signing-time-attribute"),
              stPresent ? "present" : "absent")
        << "the document's signing-time declaration disagrees with the "
           "container";

    // --- 3. A B-B claim is only permissible when the container earns it. -
    // CAdES-BES needs signing-certificate-v2; PAdES needs signing-time gone;
    // the PDF layer needs the ETSI SubFilter. All three, or no claim.
    const bool bbEarned =
        (f.subFilter == "ETSI.CAdES.detached") && essPresent && !stPresent;
    EXPECT_EQ(decl.at("pades-b-b"), bbEarned ? "supported" : "not-supported")
        << "docs/signature-profile-support.md declares pades-b-b '"
        << decl.at("pades-b-b") << "' but the measured container "
        << (bbEarned ? "does" : "does not")
        << " meet the B-B prerequisites (SubFilter='" << f.subFilter
        << "', signing-certificate-v2=" << (essPresent ? "present" : "absent")
        << ", signing-time=" << (stPresent ? "present" : "absent") << ")";

    // B-T, B-LT and B-LTA are all defined ON TOP of B-B: none can be claimed
    // while B-B is not earned.
    if (!bbEarned) {
        for (const char* key : {"pades-b-t", "pades-b-lt", "pades-b-lta"}) {
            EXPECT_EQ(decl.at(key), "not-supported")
                << "docs/signature-profile-support.md declares " << key << " '"
                << decl.at(key)
                << "' while PAdES-B-B is not met; every higher baseline level "
                   "is defined on top of B-B, so this claim cannot be true";
        }
    }

    // --- 4. Document timestamp: declaration vs. product AND vs. source. --
    EXPECT_FALSE(f.hasDocumentTimestamp)
        << "signing emitted a document timestamp";
    EXPECT_EQ(decl.at("document-timestamp"),
              f.hasDocumentTimestamp ? "implemented" : "not-implemented")
        << "the document's document-timestamp declaration disagrees with the "
           "saved bytes (PDF-35)";
    if (decl.at("pades-b-lta") == "supported") {
        ADD_FAILURE()
            << "B-LTA is declared supported. B-LTA requires a document-level "
               "/DocTimeStamp, and none is emitted (PDF-35).";
    }
}

/// Recursively scan a directory for files with the given extensions and return
/// the ones whose bytes contain @p needle, plus the number of files visited.
struct ScanResult {
    std::vector<std::string> hits;   // repo-relative "path:line: text"
    int filesVisited = 0;
};

ScanResult scanForToken(const fs::path& root,
                        const std::set<std::string>& exts,
                        const std::regex& pattern) {
    ScanResult r;
    if (!fs::exists(root)) return r;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        if (!exts.count(e.path().extension().string())) continue;
        ++r.filesVisited;
        std::ifstream in(e.path());
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            if (std::regex_search(line, pattern)) {
                r.hits.push_back(
                    fs::relative(e.path(), repoRoot()).generic_string() + ":" +
                    std::to_string(n) + ": " + line);
            }
        }
    }
    return r;
}

TEST(PadesDocClaims, NoSourceCommentClaimsPadesSupport) {
    const ScanResult r = scanForToken(repoRoot() / "src",
                                      {".cpp", ".h", ".hpp", ".cc"},
                                      std::regex("PAdES"));
    ASSERT_GT(r.filesVisited, 100)
        << "the src/ scan visited only " << r.filesVisited
        << " files — it is not scanning the tree, so a zero result is vacuous";

    std::ostringstream why;
    for (const auto& h : r.hits) why << "\n  " << h;
    EXPECT_TRUE(r.hits.empty())
        << "src/ names PAdES again. PDFE does not implement PAdES (gap "
           "PDF-34); five such comments were stripped by F20 and the claim "
           "belongs in docs/signature-profile-support.md, which is checked "
           "against behaviour. Offending lines:"
        << why.str();
}

TEST(PadesDocClaims, CapabilityDocsMakeNoProfileLevelClaim) {
    const auto docs = capabilityDocs();
    ASSERT_GE(docs.size(), 4u)
        << "docs/signature-profile-support.md lists only " << docs.size()
        << " capability documents; the set was shrunk (it must at least cover "
           "README.md and the two Acrobat-parity documents)";

    // "PAdES-B-" / "PAdES B-" is the profile-LEVEL vocabulary. Only
    // docs/signature-profile-support.md may use it, because only that document
    // says the levels are unsupported.
    const std::regex levelClaim(R"(PAdES[- ]B-)");
    std::ostringstream why;
    int offenders = 0;
    for (const auto& rel : docs) {
        const fs::path p = repoRoot() / rel;
        ASSERT_TRUE(fs::exists(p))
            << "capability document " << rel
            << " listed in docs/signature-profile-support.md does not exist";
        const std::string body = readFileBytes(p);
        ASSERT_FALSE(body.empty()) << rel << " is empty";
        std::istringstream in(body);
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            if (std::regex_search(line, levelClaim)) {
                ++offenders;
                why << "\n  " << rel << ":" << n << ": " << line;
            }
        }
    }
    EXPECT_EQ(offenders, 0)
        << "a capability document names a PAdES baseline level. PDFE supports "
           "none of them (PDF-34/PDF-35); point at "
           "docs/signature-profile-support.md instead. Offending lines:"
        << why.str();
}

TEST(PadesDocClaims, ProfileSupportDocIsReachableFromTheCapabilityDocs) {
    // A disclaimer nobody can find is not a disclaimer. Each capability
    // document must point at the single source of truth.
    const auto docs = capabilityDocs();
    ASSERT_GE(docs.size(), 4u);
    for (const auto& rel : docs) {
        const std::string body = readFileBytes(repoRoot() / rel);
        ASSERT_FALSE(body.empty()) << rel << " is empty";
        EXPECT_NE(body.find("signature-profile-support.md"), std::string::npos)
            << rel << " no longer points at docs/signature-profile-support.md, "
                      "so its signature claims are unanchored again (PDF-34)";
    }
}

} // namespace
