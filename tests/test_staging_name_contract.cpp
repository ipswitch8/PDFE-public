// -*- coding: utf-8 -*-
//
// THE STAGING-NAME CONTRACT (F58-d) — pinned as a property, not as prose.
// ---------------------------------------------------------------------------
//
// PDFE writes two different kinds of file beside a save target, and the two
// mean OPPOSITE things to the crash-recovery path:
//
//   "<target>.pdfe_tmp"             a COMPLETE, pdf_save_document-finished full
//                                   rewrite that never got renamed over the
//                                   original. This IS the R2-04 recovery
//                                   artifact — it is surfaced to the user as
//                                   recoverable unsaved work.
//
//   "<target>.pdfe_part.<32 hex>"   a POSSIBLY TRUNCATED incremental append,
//                                   held behind an exclusive handle. Internal
//                                   residue. Swept, NEVER surfaced.
//
// F58-d exists because those two shapes were once the SAME name. A real
// TerminateProcess mid-append left a 46 MB ".pdfe_tmp" whose only `startxref`
// was the ORIGINAL's — it opened as the pre-save document with 46 MB of
// unsigned junk past the last `%%EOF`, which for a signed document is the
// classic incremental-update tampering shape. It was offered to the user as
// "your unsaved changes".
//
// The fix was to make the two distinguishable BY NAME. That fix is only as good
// as the predicates that read the names, so this file tests them directly
// rather than through a save. The load-bearing case is the last one:
// MUTUAL EXCLUSIVITY. If any single name could ever satisfy both predicates,
// the distinction is gone and the defect is back — no matter how green every
// save-level test is.
//
// KILLS: replacing either predicate with a loose `ends_with`/substring match;
// giving the staged appends the ".pdfe_tmp" suffix again; a classifier that
// answers a bare "is this staging residue?" and drops the kind.

#include <gtest/gtest.h>

#include "core/AtomicFileReplace.h"

#include <filesystem>
#include <string>
#include <vector>

using pdfe::core::classifyStagingName;
using pdfe::core::fullRewriteTempPathFor;
using pdfe::core::isFullRewriteTempName;
using pdfe::core::kFullRewriteTempSuffix;
using pdfe::core::StagingFile;
using pdfe::core::StagingNameKind;

namespace {

const std::string kTarget = "doc.pdf";

// A syntactically valid staged-append residue name for `kTarget`: exactly 32
// lowercase hex digits after the marker, which is what StagingFile produces
// from 128 CSPRNG bits.
std::string partName(const std::string& hex32, const std::string& target = kTarget)
{
    return target + StagingFile::kSuffixMarker + hex32;
}

const std::string kHex32 = "0123456789abcdef0123456789abcdef";

}  // namespace

// ---------------------------------------------------------------------------
// fullRewriteTempPathFor — the one definition of the name
// ---------------------------------------------------------------------------

TEST(StagingNameContract, FullRewritePathAppendsToTheWholePathNotTheExtension)
{
    const std::filesystem::path target =
        std::filesystem::path("C:") / "work" / "doc.pdf";
    const auto temp = fullRewriteTempPathFor(target);

    // Appended, never substituted: replace_extension would produce
    // "doc.pdfe_tmp", which collides with a plausible user file and is NOT the
    // name Document::save has ever written.
    EXPECT_EQ(temp.filename().string(), std::string("doc.pdf") + kFullRewriteTempSuffix);

    // Same directory as the target — the eventual rename must be a single
    // atomic directory operation, which it cannot be across volumes.
    EXPECT_EQ(temp.parent_path(), target.parent_path());

    // And the product's own predicate must recognise what the product's own
    // path builder produced. These two drifting apart is the whole failure
    // mode this file exists to prevent.
    EXPECT_TRUE(isFullRewriteTempName(target.filename(), temp.filename()));
}

// ---------------------------------------------------------------------------
// isFullRewriteTempName — the RECOVERABLE shape, exactly
// ---------------------------------------------------------------------------

TEST(StagingNameContract, FullRewritePredicateAcceptsOnlyTheExactName)
{
    EXPECT_TRUE(isFullRewriteTempName(kTarget, kTarget + kFullRewriteTempSuffix))
        << "the genuine R2-04 recovery artifact was not recognised";
}

TEST(StagingNameContract, FullRewritePredicateRejectsEverythingElse)
{
    struct Case { std::string name; const char* why; };
    const std::vector<Case> cases = {
        {partName(kHex32),
         "a staged incremental append is POSSIBLY TRUNCATED and must never be "
         "classified as the recoverable full-rewrite artifact — this is F58-d"},
        {"unrelated.txt", "a foreign file in the same directory"},
        {kTarget, "the target itself"},
        {kTarget + ".pdfe_tmp2",
         "a near miss: a suffix-only ends_with would still reject this, but a "
         "substring/starts_with match would accept it"},
        {kTarget + ".pdfe_tmp.bak",
         "a near miss with trailing content — an ends_with on the target name "
         "plus a loose tail would accept it"},
        {"other.pdf" + std::string(kFullRewriteTempSuffix),
         "the right SHAPE but the wrong TARGET — recovering this would offer "
         "the user a different document's unsaved work"},
        {std::string(kFullRewriteTempSuffix),
         "the bare suffix with no target name at all"},
        {"", "an empty name"},
        {"doc.pd" + std::string(kFullRewriteTempSuffix),
         "a truncated target name — prefix-length arithmetic done wrong lands "
         "exactly here"},
    };

    for (const auto& c : cases) {
        EXPECT_FALSE(isFullRewriteTempName(kTarget, c.name))
            << "accepted \"" << c.name << "\" as the full-rewrite temp of \""
            << kTarget << "\": " << c.why;
    }
}

// ---------------------------------------------------------------------------
// isStagingResidueName — the mirror image: the SWEPT shape, exactly
// ---------------------------------------------------------------------------

TEST(StagingNameContract, ResiduePredicateAcceptsOnlyTheHexShape)
{
    EXPECT_TRUE(StagingFile::isStagingResidueName(kTarget, partName(kHex32)));
    EXPECT_TRUE(StagingFile::isStagingResidueName(
        kTarget, partName("ffffffffffffffffffffffffffffffff")));
    EXPECT_TRUE(StagingFile::isStagingResidueName(
        kTarget, partName("00000000000000000000000000000000")));
}

TEST(StagingNameContract, ResiduePredicateRejectsEverythingElse)
{
    struct Case { std::string name; const char* why; };
    const std::vector<Case> cases = {
        {kTarget + kFullRewriteTempSuffix,
         "the COMPLETE full-rewrite artifact — sweeping this would silently "
         "DELETE the user's recoverable unsaved work, the inverse F58-d "
         "failure and a data-loss bug"},
        {partName("0123456789abcdef0123456789abcde"),
         "31 hex digits — one short. A length check written as >= would accept "
         "the wrong side of this boundary"},
        {partName("0123456789abcdef0123456789abcdef0"),
         "33 hex digits — one long"},
        {partName("0123456789abcdef0123456789abcdeg"),
         "non-hex character in the last position"},
        {partName("g123456789abcdef0123456789abcdef"),
         "non-hex character in the first position"},
        {partName("0123456789ABCDEF0123456789ABCDEF"),
         "uppercase hex — StagingFile emits lowercase, so an uppercase name is "
         "not one of ours and must not be deleted"},
        {partName(kHex32, "other.pdf"),
         "the right shape but a different target's residue — deleting it would "
         "interfere with a concurrent save of another document"},
        {kTarget + ".pdfe_part." , "the marker with no hex at all"},
        {kTarget + kHex32, "hex digits with no marker"},
        {"unrelated.txt", "a foreign file"},
        {"", "an empty name"},
    };

    for (const auto& c : cases) {
        EXPECT_FALSE(StagingFile::isStagingResidueName(kTarget, c.name))
            << "accepted \"" << c.name << "\" as staged-append residue of \""
            << kTarget << "\": " << c.why;
    }
}

// ---------------------------------------------------------------------------
// classifyStagingName — the question the recovery scan actually asks
// ---------------------------------------------------------------------------

TEST(StagingNameContract, ClassifierReturnsTheThreeKinds)
{
    EXPECT_EQ(classifyStagingName(kTarget, kTarget + kFullRewriteTempSuffix),
              StagingNameKind::CompleteFullRewrite);
    EXPECT_EQ(classifyStagingName(kTarget, partName(kHex32)),
              StagingNameKind::PartialIncrementalAppend);
    EXPECT_EQ(classifyStagingName(kTarget, "unrelated.txt"),
              StagingNameKind::NotStaging);
    EXPECT_EQ(classifyStagingName(kTarget, kTarget),
              StagingNameKind::NotStaging);
    EXPECT_EQ(classifyStagingName(kTarget, partName(kHex32, "other.pdf")),
              StagingNameKind::NotStaging);
}

TEST(StagingNameContract, ClassifierAgreesWithBothPredicatesOnEveryInput)
{
    // The classifier must be a pure restatement of the two predicates. If it
    // ever grows its own matching logic, it can disagree with the code that
    // actually sweeps and actually recovers — and then the classification a
    // reader trusts is not the one the product acts on.
    const std::vector<std::string> corpus = {
        kTarget,
        kTarget + kFullRewriteTempSuffix,
        kTarget + ".pdfe_tmp2",
        partName(kHex32),
        partName("0123456789abcdef0123456789abcde"),
        partName("0123456789ABCDEF0123456789ABCDEF"),
        partName(kHex32, "other.pdf"),
        "other.pdf" + std::string(kFullRewriteTempSuffix),
        std::string(kFullRewriteTempSuffix),
        "unrelated.txt",
        "",
    };

    for (const auto& name : corpus) {
        const bool full = isFullRewriteTempName(kTarget, name);
        const bool part = StagingFile::isStagingResidueName(kTarget, name);
        const auto kind = classifyStagingName(kTarget, name);

        const StagingNameKind expected =
            full ? StagingNameKind::CompleteFullRewrite
                 : (part ? StagingNameKind::PartialIncrementalAppend
                         : StagingNameKind::NotStaging);
        EXPECT_EQ(kind, expected)
            << "classifier disagreed with the predicates for \"" << name << "\"";
    }
}

// ---------------------------------------------------------------------------
// THE PROPERTY THAT IS F58-d
// ---------------------------------------------------------------------------

TEST(StagingNameContract, TheTwoKindsAreMutuallyExclusiveForEveryInput)
{
    // A name that satisfied BOTH predicates would be simultaneously "offer this
    // to the user as their recovered document" and "delete this without asking"
    // — the exact collapse F58-d closed. Exhaustive over a corpus built to
    // straddle every boundary either predicate has: suffix, marker, hex
    // alphabet, hex length, target identity, and the two shapes nested inside
    // one another.
    std::vector<std::string> corpus = {
        "",
        kTarget,
        "unrelated.txt",
        std::string(kFullRewriteTempSuffix),
        std::string(StagingFile::kSuffixMarker),
        std::string(StagingFile::kSuffixMarker) + kHex32,
        kTarget + kFullRewriteTempSuffix,
        kTarget + kFullRewriteTempSuffix + kFullRewriteTempSuffix,
        kTarget + ".pdfe_tmp2",
        kTarget + ".pdfe_tm",
        kTarget + ".PDFE_TMP",
        partName(kHex32),
        partName(""),
        partName("abc"),
        partName("0123456789abcdef0123456789abcde"),    // 31
        partName("0123456789abcdef0123456789abcdef0"),  // 33
        partName("0123456789ABCDEF0123456789ABCDEF"),   // uppercase
        partName("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"),   // non-hex
        partName(kHex32, "other.pdf"),
        partName(kHex32, kTarget + kFullRewriteTempSuffix),  // one nested in the other
        (kTarget + kFullRewriteTempSuffix) + StagingFile::kSuffixMarker + kHex32,
        "other.pdf" + std::string(kFullRewriteTempSuffix),
        "doc.pd" + std::string(kFullRewriteTempSuffix),
    };

    // Also sweep every hex length either side of 32 with a valid alphabet, so
    // the exclusivity claim is not resting on three hand-picked lengths.
    for (std::size_t n = 0; n <= 40; ++n) {
        corpus.push_back(partName(std::string(n, 'a')));
    }

    for (const auto& name : corpus) {
        const bool full = isFullRewriteTempName(kTarget, name);
        const bool part = StagingFile::isStagingResidueName(kTarget, name);
        EXPECT_FALSE(full && part)
            << "\"" << name << "\" is classified as BOTH a complete, "
               "recoverable full rewrite AND a possibly-truncated staged "
               "append. That collapses the F58-d distinction: the same file "
               "would be both offered to the user and silently deleted.";
    }
}
