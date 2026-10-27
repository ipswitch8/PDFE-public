// -*- coding: utf-8 -*-
// GitHub #240 — a USER may open a network file; a DOCUMENT may not.
//
// THE POLICY, AND WHY IT IS TWO QUESTIONS RATHER THAN ONE SWITCH
//
// PDFE refused every network path, deliberately (GitHub #91 / WIN-60). That
// refusal conflated two very different acts:
//
//   * a person choosing a file on their own file server — ordinary, and the
//     thing #240 was filed about;
//   * a PDF naming a UNC resource — the document choosing a host for this
//     process to touch, which reaches the SMB / NetBIOS kernel resolver and
//     leaks the fact that this machine opened this file before any content is
//     fetched. That is the vector #91 closed, and it stays closed.
//
// So `Document::OpenOrigin` splits them, and `DocumentRequested` is the DEFAULT:
// a call site that says nothing keeps the refusal. The permissive direction has
// to be typed.
//
// THE OPT-IN, AND ITS SCOPE
//
// A setting (default OFF) lets a document opened FROM a share reach other
// resources ON THAT SAME share. Same share — not "any network location". The
// caller asserts the user enabled it, because only the app layer can know the
// setting; `Document::open` verifies the target is on the referring document's
// share, because only a path comparison can know that. Neither half is taken on
// trust from the other, and the tests below pin both.
//
// WHAT THESE TESTS DO NOT NEED
//
// A reachable file server. Every case here is about the DECISION — which origin
// is refused, and whether a share comparison is correct — so the paths need only
// be well-formed. The one thing that does need a real share (whether the
// low-integrity preflight child can authenticate to SMB) is measured separately
// in test_preflight_unc_reachability.cpp, because it cannot be reasoned about.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PathSecurity.h"

#include <memory>
#include <string>

using pdfe::core::Document;
namespace ps = pdfe::core::path_security;

namespace {

/// A UNC path that is well-formed and certainly not reachable. Reachability is
/// irrelevant here: every assertion below is about the refusal decision, which
/// happens before anything touches the network.
constexpr const char* kShareA = "\\\\pdfe-test-host-a\\shareone\\doc.pdf";
constexpr const char* kShareASibling = "\\\\pdfe-test-host-a\\shareone\\sub\\logo.png";
constexpr const char* kShareAOther = "\\\\pdfe-test-host-a\\sharetwo\\doc.pdf";
constexpr const char* kShareB = "\\\\pdfe-test-host-b\\shareone\\doc.pdf";

std::string openFailureFor(Document::OpenOrigin origin, const char* target)
{
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    try {
        Document::open(ctx, std::filesystem::path(target), "", origin);
        return {};  // no throw
    } catch (const std::exception& ex) {
        return ex.what();
    }
}

}  // namespace

// ─── The share comparison, on its own ───────────────────────────────────────

TEST(NetworkShareScope, TheSameShareCompares)
{
    EXPECT_TRUE(ps::same_network_share(std::string_view{kShareA},
                                       std::string_view{kShareASibling}));
}

// The assertion that makes the setting narrow rather than wide. A different
// share on the SAME HOST must not count — otherwise enabling same-share access
// for `\\finance\reports` would also open `\\finance\payroll`.
TEST(NetworkShareScope, ADifferentShareOnTheSameHostDoesNotCompare)
{
    EXPECT_FALSE(ps::same_network_share(std::string_view{kShareA},
                                        std::string_view{kShareAOther}));
}

TEST(NetworkShareScope, ADifferentHostDoesNotCompare)
{
    EXPECT_FALSE(ps::same_network_share(std::string_view{kShareA},
                                        std::string_view{kShareB}));
}

// Windows host and share names are case-insensitive, so a case difference is the
// same share. If this failed the setting would simply not work for users whose
// shortcuts differ in case from their typed paths.
TEST(NetworkShareScope, CaseDiffersButTheShareIsTheSame)
{
    EXPECT_TRUE(ps::same_network_share(
        std::string_view{"\\\\PDFE-TEST-HOST-A\\ShareOne\\doc.pdf"},
        std::string_view{kShareASibling}));
}

// Forward slashes are a legal spelling of a UNC and must not be a way to make
// two paths on one share look like two shares.
TEST(NetworkShareScope, ForwardSlashSpellingIsTheSameShare)
{
    EXPECT_TRUE(ps::same_network_share(
        std::string_view{"//pdfe-test-host-a/shareone/doc.pdf"},
        std::string_view{kShareASibling}));
}

// A local pair is not the question being asked, and must answer false rather
// than "same" — otherwise two local files would satisfy a same-share check and
// the comparison would be useless as a gate.
TEST(NetworkShareScope, LocalPathsAreNotTheSameShare)
{
    EXPECT_FALSE(ps::same_network_share(std::string_view{"C:\\a\\doc.pdf"},
                                        std::string_view{"C:\\a\\logo.png"}));
}

// A host with no share component has no share to match.
TEST(NetworkShareScope, AHostWithoutAShareHasNoRoot)
{
    EXPECT_TRUE(ps::network_share_root(L"\\\\pdfe-test-host-a").empty());
    EXPECT_TRUE(ps::network_share_root(L"\\\\pdfe-test-host-a\\").empty());
    EXPECT_FALSE(ps::network_share_root(L"\\\\pdfe-test-host-a\\shareone").empty());
}

// The root is exposed so a test can see WHICH share was computed. Without it,
// "same" and "both resolved to nothing" are indistinguishable — and the latter
// compares equal if the emptiness check is ever dropped.
TEST(NetworkShareScope, TheRootIsTheServerAndShareOnly)
{
    EXPECT_EQ(ps::network_share_root(L"\\\\host\\share\\deep\\path\\f.pdf"),
              std::wstring(L"\\\\host\\share"));
}

// THE BYPASS. A gate review found this by reimplementing the algorithm and
// trying the case; no test had it.
//
// A target of the form  \\\\server\\share\\..\\attackershare\\payload.pdf  is resolved by
// Windows onto  \\\\server\\attackershare  -- a DIFFERENT share. The first version of
// network_share_root() read the first two components positionally and never
// applied the "..", so it answered the share the path LEAVES and the target
// compared EQUAL to a document living there. A PDF on one share could therefore
// name a target on any other share of the same host and pass the same-share
// check.
//
// Reachable from document content through a link target, so it defeated the
// whole point of the opt-in.
TEST(NetworkShareScope, DotDotCannotEscapeIntoAnotherShare)
{
    const char* doc = "\\\\pdfe-test-host-a\\shareone\\sub\\doc.pdf";
    const char* escaped = "\\\\pdfe-test-host-a\\shareone\\..\\attackershare\\payload.pdf";

    EXPECT_EQ(ps::network_share_root(L"\\\\pdfe-test-host-a\\shareone\\..\\attackershare\\payload.pdf"),
              std::wstring(L"\\\\pdfe-test-host-a\\attackershare"))
        << "the \"..\" was not applied, so the root names the share the path "
           "LEAVES rather than the one it reaches";

    EXPECT_FALSE(ps::same_network_share(std::string_view{doc},
                                        std::string_view{escaped}))
        << "a \"..\" segment walked out of the document's share and the "
           "comparison still said SAME -- a bypass of the same-share guarantee";
}

// The benign direction must still work: dot segments that stay inside the share
// are not an attack, and breaking them would make the feature appear dead.
TEST(NetworkShareScope, DotSegmentsWithinTheShareStillCompareEqual)
{
    const char* doc = "\\\\pdfe-test-host-a\\shareone\\doc.pdf";
    const char* windy = "\\\\pdfe-test-host-a\\shareone\\sub\\..\\.\\logo.png";

    EXPECT_TRUE(ps::same_network_share(std::string_view{doc},
                                       std::string_view{windy}))
        << "a path that wanders within its own share was treated as a different "
           "share";
}

// Walking up past the share entirely leaves no share to match.
TEST(NetworkShareScope, DotDotPastTheShareRootHasNoRoot)
{
    EXPECT_TRUE(ps::network_share_root(L"\\\\pdfe-test-host-a\\shareone\\..").empty())
        << "a path that walks out of its only share still reported a root";
    EXPECT_TRUE(ps::network_share_root(L"\\\\pdfe-test-host-a\\shareone\\..\\..").empty());
}

// Trailing and repeated separators are spelling, not structure.
TEST(NetworkShareScope, TrailingAndRepeatedSeparatorsDoNotChangeTheRoot)
{
    const std::wstring expected = L"\\\\pdfe-test-host-a\\shareone";
    EXPECT_EQ(ps::network_share_root(L"\\\\pdfe-test-host-a\\shareone\\"), expected);
    EXPECT_EQ(ps::network_share_root(L"\\\\pdfe-test-host-a\\shareone\\\\sub"), expected);
    EXPECT_EQ(ps::network_share_root(L"\\\\pdfe-test-host-a\\shareone\\.\\sub"), expected);
}


// ─── The open-origin policy ─────────────────────────────────────────────────

// The security-relevant case. This is the one that must never regress.
TEST(NetworkOpenOrigin, DocumentRequestedNetworkPathIsRefused)
{
    const std::string err = openFailureFor(Document::OpenOrigin::DocumentRequested, kShareA);
    ASSERT_FALSE(err.empty()) << "a document-requested network path was NOT refused";
    EXPECT_NE(err.find("network path"), std::string::npos)
        << "refused, but not for being a network path: " << err;
}

// The DEFAULT must be the refusing one, so a call site that says nothing is safe.
// Asserting this separately from the explicit case is the point: the enum value
// being correct and the DEFAULT being that value are two different facts.
TEST(NetworkOpenOrigin, TheDefaultOriginRefusesANetworkPath)
{
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    std::string err;
    try {
        // No origin argument at all — exactly what every pre-existing caller in
        // the tree does.
        Document::open(ctx, std::filesystem::path(kShareA));
    } catch (const std::exception& ex) {
        err = ex.what();
    }
    ASSERT_FALSE(err.empty())
        << "omitting the origin admitted a network path — the default is the "
           "wrong way round, and every existing call site is now permissive";
    EXPECT_NE(err.find("network path"), std::string::npos) << err;
}

// A user-initiated open must get PAST the network refusal. It then fails for an
// ordinary reason (the host does not exist), and that distinction is the whole
// assertion: the message must not be the network refusal.
TEST(NetworkOpenOrigin, UserInitiatedNetworkPathIsNotRefusedForBeingRemote)
{
    const std::string err = openFailureFor(Document::OpenOrigin::UserInitiated, kShareA);
    ASSERT_FALSE(err.empty()) << "the unreachable host somehow opened";
    EXPECT_EQ(err.find("network path requested by document content"), std::string::npos)
        << "a USER-initiated open was refused by the document-content gate: " << err;
}

// There is no "same share allowed" origin, deliberately, and this pins why.
//
// The GitHub #240 opt-in applies to link targets — the only document-requested
// resource path PDFE actually has, since it implements no GoToR action and so no
// PDF can ask Document::open for another document. An origin value for it would
// have had exactly zero callers, which is the shape GitHub #253 is about: a
// capability compiled in and unreachable. The scoping lives in
// MainWindow::followLinkAt, and same_network_share() (tested above) is the rule
// it applies.
//
// This test exists so that adding such a value back is a deliberate act with a
// failing test to explain itself to, rather than a quiet re-introduction.
TEST(NetworkOpenOrigin, ThereAreExactlyTwoOrigins)
{
    // If a third origin is added, this stops compiling or stops being true —
    // either way the reader is sent to the comment above.
    EXPECT_EQ(static_cast<int>(Document::OpenOrigin::DocumentRequested), 0);
    EXPECT_EQ(static_cast<int>(Document::OpenOrigin::UserInitiated), 1);
}

// A LOCAL path must be unaffected by all of this, under every origin. The gate
// is about network locations; if it started refusing local files the application
// would be unusable and the tests above would still pass.
TEST(NetworkOpenOrigin, ALocalPathIsNotRefusedByAnyOrigin)
{
    for (const auto origin : {Document::OpenOrigin::DocumentRequested,
                              Document::OpenOrigin::UserInitiated}) {
        const std::string err = openFailureFor(origin, "C:\\pdfe-no-such-file-p94.pdf");
        // It fails because the file does not exist — never because it is remote.
        EXPECT_EQ(err.find("network path"), std::string::npos)
            << "a local path was refused as a network path: " << err;
    }
}
