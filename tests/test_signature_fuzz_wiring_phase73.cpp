// -*- coding: utf-8 -*-
//
// Phase-73 (pipeline phase-20) — guards for the signature fuzz wiring.
//
// WHY A WIRING TEST AND NOT JUST THE HARNESSES. The defect this phase fixed was
// not a bug in a parser. It was that tests/test_pkcs7_fuzz.cpp declared an
// `LLVMFuzzerTestOneInput` that no target ever linked (DCA 0037), so the
// repository advertised coverage-guided PKCS#7 fuzzing it never performed. That
// class of defect is invisible to every test that runs the code, because the
// code is fine — what is broken is the BUILD AND CI WIRING around it. Only a
// test that reads the build files can see it.
//
// The same reasoning covers the dictionary and the budget: a campaign handed
// the wrong dictionary, or scheduled past its own timeout, produces a perfectly
// healthy-looking log while covering less than it claims.
//
// NOTE ON LINE ENDINGS: core.autocrlf=true in this repository, so every content
// comparison here normalises CRLF. A guard that compares raw bytes passes in the
// session that wrote the file and fails after the next fresh checkout.

#include <gtest/gtest.h>

#include "signatures/CrlParser.h"
#include "signatures/OcspResponseParser.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repoRoot() { return fs::path(PDFE_REPO_ROOT); }

std::string readNormalised(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

/// Strip C++ comments so a guard tests CODE rather than prose.
///
/// Both of this file's content guards failed on their first run against the
/// very comments that explain them: the phrase "LLVMFuzzerTestOneInput" appears
/// in test_pkcs7_fuzz.cpp's header describing the promotion, and asn1.dict's
/// header names "endobj" and "%%EOF" while explaining why PDF tokens do not
/// belong in it. Matching raw file text cannot tell a declaration from a
/// sentence about a declaration -- and the same weakness runs the other way,
/// letting a real occurrence hide inside a comment-shaped region.
std::string stripCppComments(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    enum { Code, Line, Block } st = Code;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (st == Code) {
            if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                st = Line;
                ++i;
            } else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
                st = Block;
                ++i;
            } else {
                out.push_back(s[i]);
            }
        } else if (st == Line) {
            if (s[i] == '\n') {
                st = Code;
                out.push_back('\n');
            }
        } else {  // Block
            if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '/') {
                st = Code;
                ++i;
            }
        }
    }
    return out;
}

/// Strip `#`-comment lines (libFuzzer dictionary syntax), same reasoning.
std::string stripHashComments(const std::string& s)
{
    std::string out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        std::size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i < line.size() && line[i] == '#') continue;
        out += line;
        out.push_back('\n');
    }
    return out;
}

/// Read a file with its comments removed, chosen by syntax.
///
/// USE THIS FOR EVERY CONTENT ASSERTION IN THIS FILE. The first mutation run
/// killed 9 of 11 rows; both survivors were guards reading RAW text:
///
///   * M2 commented out `pdfe_add_fuzz_target(fuzz_crl_parse` -- the target
///     stops being built, and the guard still found the string inside the
///     comment and reported it registered.
///   * M10 deleted the `#include "pkcs7_fuzz_core.h"` from the harness -- and
///     the guard found the filename in the file's own header comment.
///
/// Two guards had already been fixed for exactly this (see stripCppComments),
/// and the rest were left. That is DCA 0036's lesson happening again inside the
/// phase that cites it: the unit of remediation is the class, not the call
/// site. Everything goes through here now.
std::string readCode(const fs::path& p)
{
    const std::string raw = readNormalised(p);
    const std::string ext = p.extension().string();
    if (ext == ".cpp" || ext == ".h") return stripCppComments(raw);
    return stripHashComments(raw);  // CMakeLists.txt, *.yml, *.dict
}

std::vector<uint8_t> readBytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

/// The three harnesses whose input class is DER, not PDF.
const std::vector<std::string>& signatureHarnesses()
{
    static const std::vector<std::string> v{
        "fuzz_ocsp_response", "fuzz_crl_parse", "fuzz_pkcs7_verify",
        "fuzz_tsa_response"};
    return v;
}

std::size_t countOccurrences(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return 0;
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

/// Count the entries of the FIRST `$harnesses = @( ... )` array in the
/// windows-fuzz workflow, by parsing it. Deliberately not by reading the
/// comment that states the count -- that comment was wrong by one for several
/// phases, which is the drift this test exists to stop.
int countWorkflowHarnesses(const std::string& wf)
{
    const std::string marker = "$harnesses = @(";
    const std::size_t start = wf.find(marker);
    if (start == std::string::npos) return -1;
    const std::size_t end = wf.find("\n          )", start);
    if (end == std::string::npos) return -1;

    const std::string block = wf.substr(start, end - start);
    int n = 0;
    for (std::size_t pos = block.find("\"fuzz_"); pos != std::string::npos;
         pos = block.find("\"fuzz_", pos + 6)) {
        ++n;
    }
    return n;
}

/// Extract an integer that follows `key` on the same line.
long extractLongAfter(const std::string& hay, const std::string& key)
{
    const std::size_t pos = hay.find(key);
    if (pos == std::string::npos) return -1;
    const std::size_t eol = hay.find('\n', pos);
    const std::string line = hay.substr(pos, eol - pos);
    std::string digits;
    for (char c : line.substr(key.size())) {
        if (c >= '0' && c <= '9') {
            digits.push_back(c);
        } else if (!digits.empty()) {
            break;
        }
    }
    if (digits.empty()) return -1;
    return std::stol(digits);
}

} // namespace

// =====================================================================
// 1. The promotion is real: the target exists and is registered.
// =====================================================================

TEST(SignatureFuzzWiring, EverySignatureHarnessSourceExists)
{
    const fs::path dir = repoRoot() / "tests" / "security" / "fuzz";
    for (const auto& h : signatureHarnesses()) {
        const fs::path src = dir / (h + ".cpp");
        EXPECT_TRUE(fs::exists(src))
            << "missing harness source: " << src.string();
    }
}

TEST(SignatureFuzzWiring, EverySignatureHarnessIsARegisteredFuzzTarget)
{
    const std::string cm =
        readCode(repoRoot() / "tests" / "security" / "fuzz" / "CMakeLists.txt");
    ASSERT_FALSE(cm.empty()) << "fuzz CMakeLists.txt unreadable";

    for (const auto& h : signatureHarnesses()) {
        EXPECT_NE(cm.find("pdfe_add_fuzz_target(" + h), std::string::npos)
            << h << " is not registered via pdfe_add_fuzz_target -- a harness "
                    "source that nothing builds is exactly the DCA 0037 defect";
        // Each must link pdfe_signatures, or it is not exercising the
        // signature-validation surface at all.
        const std::size_t at = cm.find("pdfe_add_fuzz_target(" + h);
        const std::size_t close = cm.find(')', at);
        ASSERT_NE(close, std::string::npos);
        EXPECT_NE(cm.substr(at, close - at).find("pdfe_signatures"),
                  std::string::npos)
            << h << " does not link pdfe_signatures";
    }
}

TEST(SignatureFuzzWiring, Pkcs7GtestNoLongerDeclaresAnUnlinkedFuzzEntryPoint)
{
    // THE ACTUAL REGRESSION GUARD FOR DCA 0037. The entry point moved to
    // tests/security/fuzz/fuzz_pkcs7_verify.cpp. If one is ever re-added here,
    // it would again be compiled into the gtest binary and linked as a fuzzer
    // by nothing -- restoring the appearance of coverage without the coverage.
    const std::string raw =
        readNormalised(repoRoot() / "tests" / "test_pkcs7_fuzz.cpp");
    ASSERT_FALSE(raw.empty());
    // Comments stripped: the file's own header DESCRIBES the promotion and
    // necessarily names the symbol. What must not come back is a declaration.
    const std::string t = stripCppComments(raw);
    EXPECT_EQ(t.find("LLVMFuzzerTestOneInput"), std::string::npos)
        << "test_pkcs7_fuzz.cpp declares a libFuzzer entry point again; "
           "the fuzzer entry point belongs in a registered fuzz target";
}

TEST(SignatureFuzzWiring, Pkcs7BodyIsSharedByBothDrivers)
{
    // One body, two drivers. If either stops including the shared core, the
    // seed-replay oracle stops being evidence about what the campaign runs.
    const std::string gtest =
        readCode(repoRoot() / "tests" / "test_pkcs7_fuzz.cpp");
    const std::string harness = readCode(
        repoRoot() / "tests" / "security" / "fuzz" / "fuzz_pkcs7_verify.cpp");
    ASSERT_FALSE(gtest.empty());
    ASSERT_FALSE(harness.empty());

    EXPECT_NE(gtest.find("pkcs7_fuzz_core.h"), std::string::npos)
        << "the gtest driver no longer shares the core body";
    EXPECT_NE(harness.find("pkcs7_fuzz_core.h"), std::string::npos)
        << "the fuzz driver no longer shares the core body";
}

// =====================================================================
// 2. The DER input class gets a DER dictionary.
// =====================================================================

TEST(SignatureFuzzWiring, Asn1DictionaryExistsAndIsNotAPdfDictionary)
{
    const fs::path d =
        repoRoot() / "tests" / "security" / "fuzz" / "asn1.dict";
    const std::string raw = readNormalised(d);
    ASSERT_FALSE(raw.empty()) << "asn1.dict missing: " << d.string();
    // Entries only: this file's header explains why PDF tokens do not belong
    // here, and necessarily names several of them.
    const std::string s = stripHashComments(raw);

    // Positive: it carries the tag and OID entries a DER mutator needs.
    EXPECT_NE(s.find("tag_sequence="), std::string::npos);
    EXPECT_NE(s.find("oid_ocsp_basic="), std::string::npos);
    EXPECT_NE(s.find("oid_pkcs7_signed_data="), std::string::npos);
    EXPECT_NE(s.find("oid_crl_number="), std::string::npos);

    // Negative, and this is the half that matters: PDF structural tokens must
    // not have leaked in. They cannot appear in a valid DER encoding, so their
    // presence would mean the two dictionaries had been conflated.
    EXPECT_EQ(s.find("endobj"), std::string::npos);
    EXPECT_EQ(s.find("%%EOF"), std::string::npos);
    EXPECT_EQ(s.find("/Filter"), std::string::npos);
}

TEST(SignatureFuzzWiring, WorkflowGivesTheDerHarnessesTheDerDictionary)
{
    const std::string wf =
        readCode(repoRoot() / ".github" / "workflows" / "windows-fuzz.yml");
    ASSERT_FALSE(wf.empty());

    EXPECT_NE(wf.find("asn1.dict"), std::string::npos)
        << "windows-fuzz.yml never references the DER dictionary";
    // The selection must be conditional. A hardcoded single dict is what this
    // phase replaced; if it comes back, every signature campaign silently
    // reverts to splicing PDF tokens into DER.
    EXPECT_NE(wf.find("$asn1Harnesses"), std::string::npos)
        << "per-harness dictionary selection is gone";
    for (const auto& h : signatureHarnesses()) {
        const std::size_t sel = wf.find("$asn1Harnesses = @(");
        ASSERT_NE(sel, std::string::npos);
        const std::size_t close = wf.find(')', sel);
        EXPECT_NE(wf.substr(sel, close - sel).find(h), std::string::npos)
            << h << " is not in the DER-dictionary list";
    }
    EXPECT_EQ(wf.find("\"-dict=tests/security/fuzz/pdf.dict\""),
              std::string::npos)
        << "the hardcoded pdf.dict invocation is back";
}

// =====================================================================
// 3. Seeds are real DER, and are actually staged.
// =====================================================================

TEST(SignatureFuzzWiring, OcspAndCrlSeedsAreRealDerNotPlaceholders)
{
    // A corpus of only-garbage never gets past the outer SEQUENCE, so the
    // campaign explores one refusal path at healthy exec/s. At least one seed
    // per harness must be a well-formed DER SEQUENCE (tag 0x30) of a plausible
    // size -- otherwise the harness is the phase-71 shape all over again: a
    // named surface its corpus cannot reach.
    struct Case { const char* dir; const char* mustParse; };
    const Case cases[] = {
        {"fuzz_ocsp_response", "ocsp_good.der"},
        {"fuzz_crl_parse",     "crl_revoked.der"},
        {"fuzz_tsa_response",  "tst_token.der"},
    };

    for (const auto& c : cases) {
        const fs::path dir = repoRoot() / "tests" / "security" / "fuzz" /
                             "seeds" / c.dir;
        ASSERT_TRUE(fs::exists(dir)) << "seed dir missing: " << dir.string();

        const auto bytes = readBytes(dir / c.mustParse);
        ASSERT_GE(bytes.size(), 64u)
            << c.mustParse << " is too small to be a real encoding";
        EXPECT_EQ(bytes[0], 0x30)
            << c.mustParse << " does not start with a DER SEQUENCE tag";
        // Long-form length: anything this size must use it, and a valid
        // long-form first length octet has the high bit set.
        EXPECT_NE(bytes[1] & 0x80, 0)
            << c.mustParse << " does not use a long-form length";
    }
}

TEST(SignatureFuzzWiring, DerSeedsAreStagedByTheBuild)
{
    // The staging glob used to be {*.pdf, *.bin} only. A .der seed set that is
    // never copied leaves the campaign starting from an empty corpus, which
    // still reports healthy exec/s -- invisible in every summary.
    const std::string cm = readCode(repoRoot() / "tests" / "security" /
                                    "fuzz" / "CMakeLists.txt");
    ASSERT_FALSE(cm.empty());
    EXPECT_NE(cm.find("PATTERN \"*.der\""), std::string::npos)
        << "the seed staging glob does not include *.der";
}

TEST(SignatureFuzzWiring, Pkcs7SeedsComeFromTheExistingCorpusNotACopy)
{
    // Criterion 3: reuse tests/corpus/pkcs7/. One copy of those bytes, two
    // consumers (this suite's seed-replay oracle and the campaign).
    const fs::path corpus = repoRoot() / "tests" / "corpus" / "pkcs7";
    ASSERT_TRUE(fs::exists(corpus));
    int n = 0;
    for (const auto& e : fs::directory_iterator(corpus)) {
        if (e.is_regular_file()) ++n;
    }
    EXPECT_GE(n, 5) << "the pkcs7 corpus lost seeds";

    // And there must NOT be a second copy under seeds/, which would drift.
    EXPECT_FALSE(fs::exists(repoRoot() / "tests" / "security" / "fuzz" /
                            "seeds" / "fuzz_pkcs7_verify"))
        << "a duplicate pkcs7 seed dir exists; the corpus is supposed to have "
           "exactly one home";

    const std::string wf =
        readCode(repoRoot() / ".github" / "workflows" / "windows-fuzz.yml");
    EXPECT_NE(wf.find("tests\\corpus\\pkcs7"), std::string::npos)
        << "the campaign does not seed fuzz_pkcs7_verify from tests/corpus/pkcs7";
}

// =====================================================================
// 4. The campaign budget is re-derived, not asserted by comment.
// =====================================================================

TEST(SignatureFuzzWiring, CampaignFitsInsideItsOwnTimeout)
{
    // THIS IS THE ONE THAT WOULD HAVE CAUGHT THE PRE-EXISTING DRIFT. The budget
    // comment said 12 harnesses while the array held 13, so the campaign
    // actually scheduled 390 min of fuzzing against a documented 360 -- eating
    // build headroom that nothing reported. Counting the array by hand is how
    // that happens; this counts it by parsing.
    const std::string wf =
        readCode(repoRoot() / ".github" / "workflows" / "windows-fuzz.yml");
    ASSERT_FALSE(wf.empty());

    const int harnesses = countWorkflowHarnesses(wf);
    ASSERT_GT(harnesses, 0) << "could not parse the harness array";

    const long duration = extractLongAfter(
        wf, "DURATION: ${{ github.event.inputs.duration_seconds || '");
    ASSERT_GT(duration, 0) << "could not parse the DURATION default";

    const long timeoutMin = extractLongAfter(wf, "timeout-minutes: ");
    ASSERT_GT(timeoutMin, 0) << "could not parse timeout-minutes";

    // The workflow's own stated reservation: 30 min for corpus -merge=1
    // minimisation, and headroom for the configure + two-stage build.
    const long mergeMin = 30;
    const long buildHeadroomMin = 210;
    const long campaignMin = (harnesses * duration) / 60;

    EXPECT_LE(campaignMin + mergeMin + buildHeadroomMin, timeoutMin)
        << "the campaign no longer fits its timeout: " << harnesses
        << " harnesses x " << duration << " s = " << campaignMin
        << " min, +" << mergeMin << " merge +" << buildHeadroomMin
        << " build headroom, against a " << timeoutMin
        << " min timeout. Lower DURATION -- do not raise the timeout.";
}

// =====================================================================
// 5. The invariants the harnesses abort on, asserted here too.
// =====================================================================
//
// fuzz_ocsp_response.cpp and fuzz_crl_parse.cpp abort when a successful parse
// does not retain the input bytes verbatim -- the contract LtvDssWriter relies
// on when it embeds the response into /DSS without re-encoding. That check runs
// only inside the libFuzzer binaries, which the ordinary MSVC suite never
// builds, so on its own it is an oracle that lives entirely in a lane most runs
// do not execute. These two drive the SAME invariant from the committed seeds
// in the ordinary suite. Neither replaces the other: the campaign reaches
// inputs no fixture will, and the fixture runs where the campaign cannot.

TEST(SignatureParserContract, OcspSuccessfulParseRetainsExactBytes)
{
    const fs::path dir =
        repoRoot() / "tests" / "security" / "fuzz" / "seeds" / "fuzz_ocsp_response";
    const auto der = readBytes(dir / "ocsp_good.der");
    ASSERT_FALSE(der.empty());

    pdfe::signatures::OcspResponse out;
    ASSERT_TRUE(pdfe::signatures::OcspResponseParser::parseBytes(
        der.data(), der.size(), out))
        << "the committed good-OCSP seed no longer parses; the corpus has "
           "stopped being able to reach the code it was built for";
    EXPECT_EQ(out.derBytes.size(), der.size());
    EXPECT_EQ(out.derBytes, der)
        << "parseBytes did not retain the DER verbatim; LtvDssWriter would "
           "embed altered bytes into /DSS /OCSPs";
}

TEST(SignatureParserContract, CrlSuccessfulParseRetainsExactBytes)
{
    const fs::path dir =
        repoRoot() / "tests" / "security" / "fuzz" / "seeds" / "fuzz_crl_parse";
    const auto der = readBytes(dir / "crl_revoked.der");
    ASSERT_FALSE(der.empty());

    pdfe::signatures::CrlInfo out;
    ASSERT_TRUE(
        pdfe::signatures::CrlParser::parseBytes(der.data(), der.size(), out))
        << "the committed CRL seed no longer parses";
    EXPECT_EQ(out.derBytes, der)
        << "parseBytes did not retain the DER verbatim";
    EXPECT_FALSE(out.issuerDN.empty())
        << "issuer DN extraction produced nothing for a valid CRL";
}

TEST(SignatureParserContract, MalformedSeedsAreRefusedCleanly)
{
    // The malformed half of each corpus must actually be malformed. A seed set
    // whose "bad" inputs all happen to parse is a seed set that only exercises
    // the success path while looking balanced.
    const fs::path ocsp =
        repoRoot() / "tests" / "security" / "fuzz" / "seeds" / "fuzz_ocsp_response";
    const fs::path crl =
        repoRoot() / "tests" / "security" / "fuzz" / "seeds" / "fuzz_crl_parse";

    struct Case { fs::path p; };
    const Case bad[] = {
        {ocsp / "ocsp_truncated.der"},
        {ocsp / "ocsp_empty_sequence.der"},
        {crl / "crl_truncated.der"},
        {crl / "crl_wrong_outer_tag.der"},
    };

    for (const auto& c : bad) {
        const auto der = readBytes(c.p);
        ASSERT_FALSE(der.empty()) << "missing seed: " << c.p.string();
        SCOPED_TRACE(c.p.filename().string());

        if (c.p.filename().string().rfind("ocsp", 0) == 0) {
            pdfe::signatures::OcspResponse out;
            EXPECT_FALSE(pdfe::signatures::OcspResponseParser::parseBytes(
                der.data(), der.size(), out))
                << "a malformed seed parsed successfully";
        } else {
            pdfe::signatures::CrlInfo out;
            EXPECT_FALSE(pdfe::signatures::CrlParser::parseBytes(
                der.data(), der.size(), out))
                << "a malformed seed parsed successfully";
        }
    }
}

TEST(SignatureFuzzWiring, EveryHarnessArrayInTheWorkflowAgrees)
{
    // Three separate arrays (build, campaign, corpus-save). A harness added to
    // one and not the others gets built and never fuzzed, or fuzzed and never
    // cached -- both of which look fine in the summary.
    const std::string wf =
        readCode(repoRoot() / ".github" / "workflows" / "windows-fuzz.yml");
    ASSERT_FALSE(wf.empty());

    EXPECT_EQ(countOccurrences(wf, "$harnesses = @("), 3u)
        << "the workflow no longer has exactly three harness arrays; this "
           "test's assumption needs revisiting, not deleting";

    for (const auto& h : signatureHarnesses()) {
        EXPECT_EQ(countOccurrences(wf, "\"" + h + "\""), 3u)
            << h << " does not appear in all three harness arrays";
    }
}
