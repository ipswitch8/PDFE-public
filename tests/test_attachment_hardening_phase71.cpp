// -*- coding: utf-8 -*-
//
// Phase-18 (follow-up phase-71) — `src/attachments`.
//
// PDF-93 is CLOSED (phase-6 §3.3: reorder/delete silently dropped attachments)
// and is cited, not re-filed. What this phase addresses is the filename and
// extraction surface, which PDF-93 never touched.
//
// The attachment name is DOCUMENT-CONTROLLED, and `sanitizeFilename` handled
// only path separators, `:`, NUL and leading dots. Everything below is a way of
// getting a name past that.
//
// THE ONE THAT MATTERS MOST IS THE DESYNC. Win32 silently strips trailing dots
// and spaces when it creates a file, so "evil.exe. " lands on disk as
// "evil.exe" while `extract()` returns "evil.exe. ". Any caller that later
// inspects, quarantines or deletes "the file we just extracted" operates on a
// name that does not exist — and the real file survives under a name nobody is
// tracking. The bug is not the odd name; it is that the returned name and the
// on-disk name disagree.

#include <gtest/gtest.h>

#include "attachments/AttachmentManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using pdfe::attachments::AttachmentManager;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

std::string san(const std::string& s)
{
    return AttachmentManager::sanitizeFilename(s);
}

std::filesystem::path scratch(const char* name)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_attach_p71" / name;
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Minimal PDF writer with a real cross-reference table.
///
/// The name-tree shapes this phase must exercise — a /Kids that points back at
/// an ancestor, a 200-deep /Kids chain — cannot be produced through
/// AttachmentManager::add(), which only ever builds a flat /Names array. They
/// have to be authored as bytes. The xref offsets are computed rather than
/// faked: a broken xref sends MuPDF down its repair path, which rebuilds the
/// document from scratch and would quietly discard the very structure under
/// test.
class PdfBuilder {
public:
    PdfBuilder() : buf_("%PDF-1.7\n"), off_(1, 0) {}

    void obj(int num, const std::string& content)
    {
        if (static_cast<int>(off_.size()) <= num) off_.resize(num + 1, 0);
        off_[num] = buf_.size();
        buf_ += std::to_string(num) + " 0 obj\n" + content + "\nendobj\n";
    }

    std::string finish(int rootNum)
    {
        const std::size_t xrefPos = buf_.size();
        const int n = static_cast<int>(off_.size());
        buf_ += "xref\n0 " + std::to_string(n) + "\n";
        buf_ += "0000000000 65535 f \n";
        for (int i = 1; i < n; ++i) {
            char line[32];
            std::snprintf(line, sizeof(line), "%010llu 00000 n \n",
                          static_cast<unsigned long long>(off_[i]));
            buf_ += line;
        }
        buf_ += "trailer\n<< /Size " + std::to_string(n) +
                " /Root " + std::to_string(rootNum) + " 0 R >>\n"
                "startxref\n" + std::to_string(xrefPos) + "\n%%EOF\n";
        return buf_;
    }

private:
    std::string buf_;
    std::vector<std::size_t> off_;
};

/// Objects 1-3 (catalog, pages, page) plus the embedded-file objects 90/91,
/// leaving 4..89 free for whatever name-tree shape the caller wants. `efRoot`
/// is the object number the catalog's /Names /EmbeddedFiles points at.
void addSkeleton(PdfBuilder& b, int efRoot)
{
    b.obj(1, "<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles "
             + std::to_string(efRoot) + " 0 R >> >>");
    b.obj(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    b.obj(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>");
    b.obj(90, "<< /Type /Filespec /F (leaf.txt) /UF (leaf.txt) "
              "/EF << /F 91 0 R >> >>");
    b.obj(91, "<< /Type /EmbeddedFile /Length 5 >>\nstream\nhello\nendstream");
}

std::filesystem::path writePdf(const std::filesystem::path& dir,
                               const char* name, const std::string& bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto p = dir / name;
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.close();
    return p;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Criterion 3 — the desync
// ─────────────────────────────────────────────────────────────────────────────

TEST(AttachmentSanitize, TrailingDotsAndSpacesAreStrippedSoTheNameSurvivesCreation)
{
    // The exact input the criterion names.
    EXPECT_EQ(san("evil.exe. "), "evil.exe");
    EXPECT_EQ(san("evil.exe "), "evil.exe");
    EXPECT_EQ(san("evil.exe."), "evil.exe");
    EXPECT_EQ(san("evil.exe...   "), "evil.exe");
}

TEST(AttachmentSanitize, TheSanitizedNameIsTheNameThatEndsUpOnDisk)
{
    // THE ACTUAL DESYNC ORACLE, and it asserts against the filesystem rather
    // than against the sanitizer's own output. A sanitizer that agreed with
    // itself but not with Win32 would pass a string-only test.
    const auto dir = scratch("desync");

    for (const char* raw : {"evil.exe. ", "trailing... ", "spaced   ",
                            "ordinary.txt"}) {
        const std::string safe = san(raw);
        const auto target = dir / safe;

        { std::ofstream f(target); f << "x"; }

        ASSERT_TRUE(std::filesystem::exists(target))
            << "input \"" << raw << "\" sanitized to \"" << safe
            << "\", which does not exist after being written — the name we "
               "hand back is not the name Win32 created";

        // And the directory must contain EXACTLY that name. If Win32 had
        // stripped something, the entry would be present under a different
        // name and exists() on the stripped path could still succeed.
        bool foundExact = false;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            if (e.path().filename().string() == safe) foundExact = true;
        }
        EXPECT_TRUE(foundExact)
            << "no directory entry is literally named \"" << safe << "\"";
        std::error_code ec;
        std::filesystem::remove(target, ec);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Criterion 2 — device names, control chars, RTL override
// ─────────────────────────────────────────────────────────────────────────────

TEST(AttachmentSanitize, Win32DeviceNamesAreRenamedWithOrWithoutAnExtension)
{
    // "CON.txt" is still CON: Windows matches the stem, so an extension does
    // not make a device name safe.
    for (const char* dev : {"CON", "PRN", "AUX", "NUL",
                            "COM1", "COM9", "LPT1", "LPT9"}) {
        EXPECT_NE(san(dev), std::string(dev))
            << "bare device name \"" << dev << "\" passed through";
        EXPECT_NE(san(std::string(dev) + ".txt"), std::string(dev) + ".txt")
            << "device name with extension passed through";
    }
    // Case-insensitive, and after trailing-space stripping.
    EXPECT_NE(san("con"), "con");
    EXPECT_NE(san("CoN.txt"), "CoN.txt");
    EXPECT_NE(san("CON "), "CON");
}

TEST(AttachmentSanitize, NamesThatMerelyStartWithADeviceNameAreLeftAlone)
{
    // The control. A rule that renamed anything CONTAINING "con" would satisfy
    // the case above while mangling ordinary files.
    EXPECT_EQ(san("console.log"), "console.log");
    EXPECT_EQ(san("connections.csv"), "connections.csv");
    EXPECT_EQ(san("com10.txt"), "com10.txt")   // COM10 is not a device name
        << "COM10 is not reserved; only COM1-COM9 are";
    EXPECT_EQ(san("report.pdf"), "report.pdf");
}

TEST(AttachmentSanitize, ControlCharactersAreReplaced)
{
    std::string withNewline = "note";
    withNewline.push_back('\n');
    withNewline += "injected.txt";
    const std::string out = san(withNewline);
    EXPECT_EQ(out.find('\n'), std::string::npos)
        << "a newline survived into a filename; it can carry a forged line "
           "into any log that records the extracted name";

    std::string withEsc = "a";
    withEsc.push_back('\x1b');          // ESC — terminal escape sequences
    withEsc += "[31mred.txt";
    EXPECT_EQ(san(withEsc).find('\x1b'), std::string::npos);

    std::string withDel = "a";
    withDel.push_back('\x7f');
    EXPECT_EQ(san(withDel).find('\x7f'), std::string::npos);
}

TEST(AttachmentSanitize, RightToLeftOverrideIsRemoved)
{
    // U+202E RIGHT-TO-LEFT OVERRIDE (UTF-8 E2 80 AE) reverses how the rest of
    // the name RENDERS. "doc<U+202E>gnp.exe" displays as "docexe.png": the user
    // sees a PNG and double-clicks an executable. The bytes are untouched, so
    // nothing that inspects the extension notices.
    const std::string rtl = "doc\xE2\x80\xAE" "gnp.exe";
    const std::string out = san(rtl);
    EXPECT_EQ(out.find("\xE2\x80\xAE"), std::string::npos)
        << "the RTL override survived sanitization";
    EXPECT_EQ(out, "docgnp.exe")
        << "the override should be dropped, leaving the real name visible";

    // The other bidi controls in the same family.
    for (const char* seq : {"\xE2\x80\xAA", "\xE2\x80\xAB", "\xE2\x80\xAD",
                            "\xE2\x81\xA6", "\xE2\x81\xA9",
                            "\xE2\x80\x8E", "\xE2\x80\x8F"}) {
        const std::string s = std::string("a") + seq + "b.txt";
        EXPECT_EQ(san(s), "ab.txt") << "bidi control not removed";
    }
}

TEST(AttachmentSanitize, OrdinaryNonAsciiNamesAreNotMangled)
{
    // The control for the bidi stripping: a rule that ate multi-byte UTF-8
    // generally would satisfy the case above and destroy every non-English
    // filename.
    // Written as explicit UTF-8 bytes: in C++20 a u8"" literal is char8_t,
    // which does not convert to std::string, and relying on the source file's
    // encoding surviving every toolchain is exactly the kind of assumption
    // that makes a test fail for reasons unrelated to what it asserts.
    const std::string accented = "rapport-\xC3\xA9t\xC3\xA9.pdf";      // été
    const std::string japanese = "\xE5\xA0\xB1\xE5\x91\x8A\xE6\x9B\xB8.pdf";
    const std::string cyrillic = "\xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB.txt";
    EXPECT_EQ(san(accented), accented);
    EXPECT_EQ(san(japanese), japanese);
    EXPECT_EQ(san(cyrillic), cyrillic);
}

TEST(AttachmentSanitize, PreExistingBehaviourIsUnchanged)
{
    // Criterion 6's concern, at the unit level: AnnotationManager reuses this
    // function, and the hardening must not break what it already guaranteed.
    EXPECT_EQ(san("../../etc/passwd"), "passwd");
    EXPECT_EQ(san("C:\\Windows\\System32\\evil.dll"), "evil.dll");
    EXPECT_EQ(san(".hidden"), "hidden");
    EXPECT_EQ(san(""), "attachment");
    EXPECT_EQ(san("."), "attachment");
    EXPECT_EQ(san(".."), "attachment");
    EXPECT_EQ(san("..."), "attachment")
        << "a name of nothing but dots must not sanitize to an empty string";
}

// ─────────────────────────────────────────────────────────────────────────────
// Criterion 5 — /Kids depth cap and cycle detection
//
// These are the only cases in this file that could previously take the process
// down rather than produce a wrong answer. `walkNameTree` recursed on /Kids
// with no depth cap and no visited set, and PDF objects are referenced by
// number, so a node whose /Kids names an ancestor is three lines of authoring
// and an unbounded recursion. A stack overflow is not an exception: no fz_catch
// and no try/catch anywhere above it helps, and it is reachable from list(),
// i.e. from merely showing a document's attachment panel.
// ─────────────────────────────────────────────────────────────────────────────

class NameTreeShapeTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx = std::make_shared<MuPdfContext>();
        dir = std::filesystem::temp_directory_path() / "pdfe_attach_p71" / "nametree";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path dir;
};

TEST_F(NameTreeShapeTest, CyclicKidsTerminatesAndReportsTruncation)
{
    // 4 -> 5 -> 4. Node 4 also carries the only real leaf, so the entry count
    // doubles as the cycle oracle: if the walk re-entered node 4 it would
    // report the same attachment again.
    PdfBuilder b;
    addSkeleton(b, 4);
    b.obj(4, "<< /Names [(leaf.txt) 90 0 R] /Kids [5 0 R] >>");
    b.obj(5, "<< /Kids [4 0 R] >>");
    const auto pdf = writePdf(dir, "cyclic.pdf", b.finish(1));

    auto doc = Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr) << "crafted PDF did not open; the fixture is wrong, "
                               "not the product";

    AttachmentManager mgr(ctx);
    const auto entries = mgr.list(doc.get());   // must RETURN at all

    EXPECT_EQ(entries.size(), 1u)
        << "the cycle was walked more than once — cycle detection is keyed on "
           "something that does not repeat";
    EXPECT_TRUE(mgr.lastTraversalTruncated())
        << "the walk stopped early but said nothing; a guard whose only "
           "evidence is 'did not crash' cannot be distinguished from a guard "
           "that never ran";
}

TEST_F(NameTreeShapeTest, DeeplyNestedKidsStopsAtTheDepthCap)
{
    // A 200-link /Kids chain with the leaf at the very bottom. The cap is 64,
    // so the leaf is unreachable: the list comes back EMPTY and truncated,
    // which is the honest answer — not a silent empty list that looks like a
    // document with no attachments.
    PdfBuilder b;
    addSkeleton(b, 10);
    const int kFirst = 10, kLast = 209;
    for (int i = kFirst; i < kLast; ++i)
        b.obj(i, "<< /Kids [" + std::to_string(i + 1) + " 0 R] >>");
    b.obj(kLast, "<< /Names [(leaf.txt) 90 0 R] >>");
    const auto pdf = writePdf(dir, "deep.pdf", b.finish(1));

    auto doc = Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);

    AttachmentManager mgr(ctx);
    const auto entries = mgr.list(doc.get());

    EXPECT_TRUE(entries.empty())
        << "the leaf sits below the depth cap and must not have been reached";
    EXPECT_TRUE(mgr.lastTraversalTruncated())
        << "an empty list from a truncated walk is indistinguishable from a "
           "document with no attachments unless the truncation is reported";
}

TEST_F(NameTreeShapeTest, LegitimateNestedTreeIsWalkedInFullAndNotFlagged)
{
    // THE CONTROL. Every assertion above is also satisfied by a walkNameTree
    // that refuses to descend /Kids at all, or that always sets the flag. A
    // real name tree IS nested — MuPDF and Acrobat both emit /Kids for more
    // than a handful of entries — so breaking nesting to satisfy the cap would
    // break attachment listing on ordinary documents.
    PdfBuilder b;
    addSkeleton(b, 4);
    b.obj(4, "<< /Kids [5 0 R] >>");
    b.obj(5, "<< /Kids [6 0 R] >>");
    b.obj(6, "<< /Names [(leaf.txt) 90 0 R] >>");
    const auto pdf = writePdf(dir, "nested.pdf", b.finish(1));

    auto doc = Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);

    AttachmentManager mgr(ctx);
    const auto entries = mgr.list(doc.get());

    ASSERT_EQ(entries.size(), 1u)
        << "a three-level name tree is ordinary; the guard must not have "
           "stopped it";
    EXPECT_EQ(entries[0].name, "leaf.txt");
    EXPECT_FALSE(mgr.lastTraversalTruncated())
        << "truncation reported for a tree that was walked in full — the flag "
           "is latched or unconditional, which makes it useless as a signal";
}

TEST_F(NameTreeShapeTest, ACycleSurvivesXrefDamageSoASeededCorpusWouldFindIt)
{
    // This test exists to settle a question about the FUZZER, not about the
    // product, and it settled it against my first answer.
    //
    // The plausible story for why `fuzz_embedded_files` never found the cycle
    // was that byte-level mutation desynchronises the xref, MuPDF repairs the
    // file, and the crafted structure is lost before the walk ever sees it.
    // That story is WRONG, and this is the measurement that shows it: repair
    // rebuilds the object graph by scanning for `N 0 obj` headers, so objects
    // 4 and 5 and their mutual /Kids references come back intact.
    //
    // Which means the real cause is the corpus, and the fix is cheap: a cyclic
    // seed would survive essentially any mutation the fuzzer applies to it, so
    // seeding one is worth far more than running the campaign longer. See
    // dca/0035.
    PdfBuilder b;
    addSkeleton(b, 4);
    b.obj(4, "<< /Names [(leaf.txt) 90 0 R] /Kids [5 0 R] >>");
    b.obj(5, "<< /Kids [4 0 R] >>");
    std::string bytes = b.finish(1);

    // Damage ONLY the xref offsets, leaving every object byte intact — which is
    // what a byte mutation of any structural field amounts to: the body and the
    // table stop agreeing.
    const auto x = bytes.find("xref\n0 ");
    ASSERT_NE(x, std::string::npos);
    for (std::size_t i = x; i + 10 < bytes.size(); ++i)
        if (bytes.compare(i, 10, "0000000000") != 0 &&
            bytes[i] >= '0' && bytes[i] <= '9')
            bytes[i] = '1';

    const auto pdf = writePdf(dir, "brokenxref.pdf", bytes);
    auto doc = Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr) << "the repair path should still open this file";

    AttachmentManager mgr(ctx);
    const auto entries = mgr.list(doc.get());

    EXPECT_EQ(entries.size(), 1u);
    EXPECT_TRUE(mgr.lastTraversalTruncated())
        << "the cycle did NOT survive xref repair. If this ever fails, "
           "dca/0035's causal analysis needs revisiting: it would mean a "
           "mutated cyclic seed degrades into a harmless one, and seeding the "
           "corpus would no longer be the cheap fix this test says it is";
}

TEST_F(NameTreeShapeTest, TruncationFlagIsResetPerCall)
{
    // A flag that latches on the first hostile document would then condemn
    // every document opened afterwards in the same session.
    PdfBuilder cyc;
    addSkeleton(cyc, 4);
    cyc.obj(4, "<< /Names [(leaf.txt) 90 0 R] /Kids [5 0 R] >>");
    cyc.obj(5, "<< /Kids [4 0 R] >>");
    const auto badPdf = writePdf(dir, "reset_bad.pdf", cyc.finish(1));

    PdfBuilder ok;
    addSkeleton(ok, 4);
    ok.obj(4, "<< /Names [(leaf.txt) 90 0 R] >>");
    const auto goodPdf = writePdf(dir, "reset_good.pdf", ok.finish(1));

    AttachmentManager mgr(ctx);

    auto bad = Document::open(ctx, badPdf);
    ASSERT_NE(bad, nullptr);
    mgr.list(bad.get());
    ASSERT_TRUE(mgr.lastTraversalTruncated());

    auto good = Document::open(ctx, goodPdf);
    ASSERT_NE(good, nullptr);
    const auto entries = mgr.list(good.get());
    EXPECT_EQ(entries.size(), 1u);
    EXPECT_FALSE(mgr.lastTraversalTruncated())
        << "the flag latched: one hostile document poisoned the verdict for "
           "every document listed afterwards";
}

// ─────────────────────────────────────────────────────────────────────────────
// The fuzz seed corpus — a standing control
//
// dca/0035's conclusion is that the corpus was the whole gap: the harness had
// one seed with a flat name tree, so the /Kids branch it claimed to target was
// unreachable. Three structure-hostile seeds now exist. This asserts they still
// ARE what they claim to be.
//
// Without this, the seeds can degrade silently — regenerated wrongly, truncated
// by a checkout, or "cleaned up" by someone who sees three near-identical PDFs
// and keeps one. A corpus entry that no longer contains the shape it was added
// for is worse than a missing one, because the corpus listing still reads as
// coverage. That is the same failure this whole DCA is about, one level down.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(NameTreeShapeTest, TheFuzzSeedCorpusStillContainsTheShapesItWasSeededWith)
{
    // TEST_FIXTURES_DIR is <repo>/tests/fixtures; the corpus is its sibling.
    // Derived rather than adding a second path macro, so there is only one
    // definition of where the source tree is.
    const auto seeds = std::filesystem::path(TEST_FIXTURES_DIR).parent_path() /
                       "security" / "fuzz" / "seeds" / "fuzz_embedded_files";
    ASSERT_TRUE(std::filesystem::exists(seeds)) << seeds.string();

    struct Expect { const char* file; bool truncated; std::size_t entries; };
    const Expect cases[] = {
        {"nametree_cyclic_kids.pdf", true,  1},   // cycle caught, leaf found once
        {"nametree_deep_kids.pdf",   true,  0},   // leaf below the depth cap
        {"nametree_nested_kids.pdf", false, 1},   // ordinary nesting, walked in full
    };

    for (const auto& c : cases) {
        const auto path = seeds / c.file;
        ASSERT_TRUE(std::filesystem::exists(path))
            << c.file << " is gone from the fuzz corpus; regenerate with "
               "tests/security/fuzz/seeds/make_embedded_files_seeds.py";

        auto doc = Document::open(ctx, path);
        ASSERT_NE(doc, nullptr) << c.file << " no longer opens";

        AttachmentManager mgr(ctx);
        const auto entries = mgr.list(doc.get());
        EXPECT_EQ(entries.size(), c.entries) << c.file;
        EXPECT_EQ(mgr.lastTraversalTruncated(), c.truncated)
            << c.file << " no longer has the name-tree shape it was seeded "
               "for; the corpus still lists it, so the gap dca/0035 closed "
               "would be quietly open again";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Criterion 4 — no silent overwrite, and a size cap
// ─────────────────────────────────────────────────────────────────────────────

class ExtractPolicyTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx = std::make_shared<MuPdfContext>();
        const auto src = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(src)) GTEST_SKIP() << "test_single.pdf missing";

        dir = std::filesystem::temp_directory_path() / "pdfe_attach_p71" / "extract";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);

        const auto work = dir / "work.pdf";
        std::filesystem::copy_file(src, work,
            std::filesystem::copy_options::overwrite_existing, ec);

        const auto payloadSrc = dir / "payload.txt";
        { std::ofstream f(payloadSrc, std::ios::binary); f << payload; }

        auto doc = Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        AttachmentManager mgr(ctx);
        ASSERT_TRUE(mgr.add(doc.get(), payloadSrc, "notes.txt"));
        docPath = dir / "with_attachment.pdf";
        doc->saveAs(docPath);
        ASSERT_TRUE(std::filesystem::exists(docPath));

        outDir = dir / "out";
        std::filesystem::create_directories(outDir, ec);
    }

    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path dir, docPath, outDir;
    const std::string payload = "the original attachment payload\n";
};

TEST_F(ExtractPolicyTest, ExtractionNeverOverwritesAnExistingFile)
{
    // Pre-place a file the attachment's name collides with. The old code
    // opened the target with the default truncating mode, so a document got to
    // choose which of the user's files to destroy simply by naming its
    // attachment after one.
    const auto victim = outDir / "notes.txt";
    const std::string victimContent = "USER DATA THAT MUST SURVIVE\n";
    { std::ofstream f(victim, std::ios::binary); f << victimContent; }

    auto doc = Document::open(ctx, docPath);
    ASSERT_NE(doc, nullptr);
    AttachmentManager mgr(ctx);

    const auto written = mgr.extract(doc.get(), "notes.txt", outDir);
    ASSERT_FALSE(written.empty()) << "extraction should still succeed";

    EXPECT_EQ(readAll(victim), victimContent)
        << "the pre-existing file was overwritten";
    EXPECT_NE(std::filesystem::weakly_canonical(written),
              std::filesystem::weakly_canonical(victim))
        << "extract() returned the victim's own path";
    EXPECT_EQ(readAll(written), payload)
        << "the attachment landed somewhere, but not with its own bytes";
    EXPECT_TRUE(std::filesystem::exists(written));
}

TEST_F(ExtractPolicyTest, TheSizeCapRefusesAndWritesNothing)
{
    auto doc = Document::open(ctx, docPath);
    ASSERT_NE(doc, nullptr);
    AttachmentManager mgr(ctx);

    ASSERT_GT(payload.size(), 4u);
    mgr.setMaxExtractBytes(4);          // smaller than the payload
    EXPECT_EQ(mgr.maxExtractBytes(), 4u);

    const auto written = mgr.extract(doc.get(), "notes.txt", outDir);
    EXPECT_TRUE(written.empty()) << "the cap was exceeded but a path came back";
    EXPECT_TRUE(mgr.lastExtractRefusedOversize())
        << "the refusal is invisible: an empty path also means 'no such "
           "attachment', so a caller cannot tell the user which happened";

    // Nothing partial left behind — a truncated file on disk would be worse
    // than either outcome, because it looks like a successful extraction.
    for (const auto& e : std::filesystem::directory_iterator(outDir))
        ADD_FAILURE() << "the refusal still wrote " << e.path().string();
}

TEST_F(ExtractPolicyTest, RaisingTheCapLetsTheSameExtractionThrough)
{
    // THE RECOVERY CONTROL. A cap implemented as "always refuse" satisfies the
    // test above. Only extracting successfully after the obstruction is lifted
    // separates a working cap from a broken extract(). Same lesson as the
    // latched fail-closed guard in phase-70.
    auto doc = Document::open(ctx, docPath);
    ASSERT_NE(doc, nullptr);
    AttachmentManager mgr(ctx);

    mgr.setMaxExtractBytes(4);
    ASSERT_TRUE(mgr.extract(doc.get(), "notes.txt", outDir).empty());

    mgr.setMaxExtractBytes(1024 * 1024);
    const auto written = mgr.extract(doc.get(), "notes.txt", outDir);
    ASSERT_FALSE(written.empty()) << "extraction stayed broken after the cap "
                                    "was raised";
    EXPECT_FALSE(mgr.lastExtractRefusedOversize());
    EXPECT_EQ(readAll(written), payload);
}

TEST_F(ExtractPolicyTest, ExtractionRefusesToWriteThroughAPlantedSymlink)
{
    // THE GATE-FOUND ONE. `security-audit` failed this phase because the target
    // was chosen with exists() and then opened with a truncating ofstream —
    // check-then-act. An attacker with write access to the output directory
    // plants a link at the name extraction is about to use, and the open
    // follows it, writing document-controlled bytes to the link's target under
    // PDFE's token.
    //
    // Phase-70 was gate-failed for the same shape in src/search. This test
    // exists so the third occurrence is caught by the suite rather than by a
    // gate agent reading the code.
    //
    // THE LINK MUST DANGLE, and getting this wrong made the first version of
    // this test worthless. My first draft pointed the link at an EXISTING
    // victim file — and the mutation harness showed the TOCTOU mutation
    // SURVIVING it. Of course it did: `std::filesystem::exists()` follows the
    // link, sees the target, reports true, and the old code politely picks
    // "notes (1).txt". Both implementations behave identically, so the test
    // discriminated nothing.
    //
    // A DANGLING link separates them. exists() follows it, finds no target,
    // and reports FALSE — so the old code takes that name and its truncating
    // open CREATES the attacker's chosen target through the link. CREATE_NEW
    // with FILE_FLAG_OPEN_REPARSE_POINT fails instead, because the link OBJECT
    // is already there regardless of what it points at.
    //
    // The oracle is therefore "the attacker's target was never created", not
    // "the victim's bytes are unchanged".
#if defined(_WIN32)
    const auto attackerTarget = dir / "planted_by_the_link.txt";
    std::error_code ec;
    std::filesystem::remove(attackerTarget, ec);
    ASSERT_FALSE(std::filesystem::exists(attackerTarget));

    const auto link = outDir / "notes.txt";
    std::filesystem::create_symlink(attackerTarget, link, ec);
    if (ec) {
        GTEST_SKIP() << "symlink creation needs Developer Mode or "
                        "SeCreateSymbolicLinkPrivilege: " << ec.message();
    }
    // Confirm the fixture really is the hostile shape: a link that exists as an
    // object while exists() (which follows it) reports false.
    ASSERT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(link)));
    ASSERT_FALSE(std::filesystem::exists(link))
        << "the link is not dangling, so this test cannot distinguish the "
           "raceable implementation from the fixed one";

    auto doc = Document::open(ctx, docPath);
    ASSERT_NE(doc, nullptr);
    AttachmentManager mgr(ctx);

    const auto written = mgr.extract(doc.get(), "notes.txt", outDir);

    EXPECT_FALSE(std::filesystem::exists(attackerTarget))
        << "extraction wrote through a planted dangling symlink and created "
           "the attacker's chosen target: " << attackerTarget.string();
    if (!written.empty()) {
        EXPECT_TRUE(std::filesystem::is_regular_file(
                        std::filesystem::symlink_status(written)))
            << "extract() returned a path that is not a regular file it made";
    }
#else
    GTEST_SKIP() << "Win32 CREATE_NEW / reparse-point semantics";
#endif
}

TEST_F(ExtractPolicyTest, ExtractedFilesCarryMarkOfTheWeb)
{
    auto doc = Document::open(ctx, docPath);
    ASSERT_NE(doc, nullptr);
    AttachmentManager mgr(ctx);

    const auto written = mgr.extract(doc.get(), "notes.txt", outDir);
    ASSERT_FALSE(written.empty());

#if defined(_WIN32)
    const std::wstring ads = written.wstring() + L":Zone.Identifier";
    std::ifstream zone(ads, std::ios::binary);
    ASSERT_TRUE(zone.good())
        << "no Zone.Identifier stream on the extracted file; without it "
           "SmartScreen and Protected View treat a file lifted out of a "
           "document as locally authored";
    std::ostringstream ss;
    ss << zone.rdbuf();
    const std::string mark = ss.str();
    EXPECT_NE(mark.find("[ZoneTransfer]"), std::string::npos);
    EXPECT_NE(mark.find("ZoneId=3"), std::string::npos) << "mark reads: " << mark;
#else
    GTEST_SKIP() << "Zone.Identifier is an NTFS alternate data stream";
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Criterion 6 — the AnnotationManager call sites still behave
//
// sanitizeFilename is shared with AnnotationManager (the /FileAttachment annot
// add and extract paths). The hardening widened what it rewrites, so the
// question is not "does it still compile" but "does a name that used to pass
// through unchanged still pass through unchanged". The aggregate suite result
// does not answer that: it would stay green if these sites had simply stopped
// being exercised.
// ─────────────────────────────────────────────────────────────────────────────

TEST(AttachmentSanitize, NamesTheAnnotationSitesRelyOnAreUnchanged)
{
    // The shapes AnnotationManager actually feeds it: a display name, a
    // filesystem basename, and its own fallback when the filespec carries
    // no name at all.
    EXPECT_EQ(san("attachment.bin"), "attachment.bin")
        << "AnnotationManager's no-name fallback must survive its own sanitizer";
    EXPECT_EQ(san("Meeting Notes 2026.docx"), "Meeting Notes 2026.docx")
        << "interior spaces are not trailing spaces";
    EXPECT_EQ(san("v1.2.3-final.pdf"), "v1.2.3-final.pdf")
        << "interior dots are not trailing dots";
    EXPECT_EQ(san("data.tar.gz"), "data.tar.gz");
    EXPECT_EQ(san("_leading_underscore.txt"), "_leading_underscore.txt");
}
