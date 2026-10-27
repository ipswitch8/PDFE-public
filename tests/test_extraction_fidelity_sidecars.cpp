// -*- coding: utf-8 -*-
//
// Extraction fidelity against committed sidecars — meta-audit follow-up F14.
//
// Gaps: PDF-70 (RTL extraction order explicitly not asserted), PDF-74
// (CID/CMap correctness only ever probed with EXPECT_NO_THROW), PDF-78
// (combining marks / normalisation untested; i18n_combining.pdf was an unused
// fixture), PDF-79 (`*.expected.json` sidecars that nothing read).
//
// THE ORACLE. Each sidecar under tests/fixtures/extraction/ carries two
// sequences per laid-out text band:
//
//   authored_codepoints   — the literal the FIXTURE GENERATOR wrote
//                           (tests/fixtures/gen_i18n_render_corpus.py), and
//   expected_codepoints   — what extraction must return,
//
// plus `presentation_form_folding`, the Unicode compatibility decomposition of
// each Arabic Presentation Form-B that shaping legitimately introduces.
//
// The test asserts BOTH directions:
//   1. extraction returns exactly `expected_codepoints`  (stability), and
//   2. folding that through the table reproduces `authored_codepoints`
//      exactly and IN ORDER                              (correctness).
//
// (2) is what makes the sidecar more than a snapshot: a reversed Hebrew run, a
// mis-segmented Arabic run, or a CID→Unicode regression that returns plausible
// but wrong characters all fail it. The pre-existing
// `RtlScriptsPreservedByUnicodeBlock` only counted codepoints per Unicode
// block and its own comment disclaimed order.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextExtractor.h"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pdfe::core;

namespace {

fs::path fixturesDir() { return fs::path(TEST_FIXTURES_DIR); }
fs::path sidecarDir()  { return fixturesDir() / "extraction"; }

/// "U+05E9" → 0x05E9. Returns -1 for anything malformed, so a corrupted sidecar
/// fails loudly rather than silently comparing garbage.
int parseCodepoint(const QString& s) {
    if (!s.startsWith("U+") || s.size() < 3) return -1;
    bool ok = false;
    const uint v = s.mid(2).toUInt(&ok, 16);
    return ok ? static_cast<int>(v) : -1;
}

std::vector<int> codepointsOf(const QString& s) {
    std::vector<int> out;
    for (const QChar& c : s) {
        // QString is UTF-16; recombine surrogate pairs so astral-plane
        // characters compare as one codepoint, not two units.
        const ushort u = c.unicode();
        if (QChar::isHighSurrogate(u) && !out.empty()) { /* handled below */ }
        out.push_back(static_cast<int>(u));
    }
    // Second pass: fold surrogate pairs.
    std::vector<int> folded;
    for (size_t i = 0; i < out.size(); ++i) {
        if (QChar::isHighSurrogate(static_cast<ushort>(out[i]))
            && i + 1 < out.size()
            && QChar::isLowSurrogate(static_cast<ushort>(out[i + 1]))) {
            folded.push_back(QChar::surrogateToUcs4(
                static_cast<ushort>(out[i]), static_cast<ushort>(out[i + 1])));
            ++i;
        } else {
            folded.push_back(out[i]);
        }
    }
    return folded;
}

std::string describe(const std::vector<int>& cps) {
    std::string s;
    for (int c : cps) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "U+%04X ", static_cast<unsigned>(c));
        s += buf;
    }
    if (!s.empty()) s.pop_back();
    return s;
}

std::vector<int> expectedList(const QJsonArray& arr, bool* ok) {
    std::vector<int> out;
    *ok = true;
    for (const auto& v : arr) {
        const int cp = parseCodepoint(v.toString());
        if (cp < 0) { *ok = false; return out; }
        out.push_back(cp);
    }
    return out;
}

/// Apply the sidecar's presentation-form folding table to a codepoint sequence.
std::vector<int> applyFolding(const std::vector<int>& cps,
                              const QJsonObject& table) {
    std::vector<int> out;
    for (int cp : cps) {
        char key[16];
        std::snprintf(key, sizeof(key), "U+%04X", static_cast<unsigned>(cp));
        const auto it = table.constFind(QString::fromLatin1(key));
        if (it == table.constEnd()) { out.push_back(cp); continue; }
        // The value is a concatenation of "U+XXXX" tokens.
        const QString mapped = it.value().toString();
        for (int i = 0; i + 2 <= mapped.size(); ) {
            const int plus = mapped.indexOf("U+", i);
            if (plus < 0) break;
            int end = mapped.indexOf("U+", plus + 2);
            if (end < 0) end = mapped.size();
            const int cp2 = parseCodepoint(mapped.mid(plus, end - plus));
            if (cp2 >= 0) out.push_back(cp2);
            i = end;
        }
    }
    return out;
}

/// Non-blank extracted lines of page 0, in extraction order.
QStringList extractLines(const std::shared_ptr<MuPdfContext>& ctx,
                         const fs::path& pdf) {
    auto doc = Document::open(ctx, pdf);
    EXPECT_NE(doc, nullptr);
    if (!doc) return {};
    auto page = doc->page(0);
    EXPECT_NE(page, nullptr);
    if (!page) return {};

    TextExtractor extractor(ctx);
    const std::string plain = extractor.extractPlainText(page.get());

    QStringList lines;
    for (const QString& raw :
         QString::fromStdString(plain).split('\n', Qt::SkipEmptyParts)) {
        const QString trimmed = raw.trimmed();
        if (!trimmed.isEmpty()) lines << trimmed;
    }
    return lines;
}

class ExtractionFidelitySidecarTest
    : public ::testing::TestWithParam<const char*> {
protected:
    void SetUp() override { ctx_ = std::make_shared<MuPdfContext>(); }
    void TearDown() override { ctx_.reset(); }
    std::shared_ptr<MuPdfContext> ctx_;
};

} // namespace

// ===========================================================================
// Criterion 5 — extraction asserted against the committed sidecars
// ===========================================================================

TEST_P(ExtractionFidelitySidecarTest, MatchesCommittedSidecar) {
    const std::string stem = GetParam();
    const fs::path sidecarPath = sidecarDir() / (stem + ".expected.json");
    ASSERT_TRUE(fs::exists(sidecarPath))
        << "committed extraction sidecar missing: " << sidecarPath;

    QFile f(QString::fromStdString(sidecarPath.string()));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << "cannot read " << sidecarPath;
    QJsonParseError perr{};
    const QJsonDocument jd = QJsonDocument::fromJson(f.readAll(), &perr);
    ASSERT_EQ(perr.error, QJsonParseError::NoError)
        << "sidecar is not valid JSON: "
        << perr.errorString().toStdString();
    const QJsonObject root = jd.object();
    ASSERT_EQ(root.value("schema").toString().toStdString(),
              "pdfe.extraction-fidelity/1");

    const fs::path pdf = fixturesDir()
        / root.value("fixture").toString().toStdString();
    ASSERT_TRUE(fs::exists(pdf)) << "fixture missing: " << pdf;

    const QJsonArray wantLines = root.value("lines").toArray();
    ASSERT_FALSE(wantLines.isEmpty()) << "sidecar declares no lines";

    const QStringList got = extractLines(ctx_, pdf);
    ASSERT_EQ(static_cast<int>(got.size()), wantLines.size())
        << "extraction produced " << got.size() << " non-blank lines, the "
           "sidecar declares " << wantLines.size()
        << ". Got: " << got.join(" | ").toStdString();

    // Forbidden codepoints (U+FFFD) — a replacement char means the CMap /
    // ToUnicode path failed, which is exactly the silent degradation PDF-74
    // and PDF-72 describe.
    std::vector<int> forbidden;
    for (const auto& v : root.value("forbidden_codepoints").toArray()) {
        const int cp = parseCodepoint(v.toString());
        if (cp >= 0) forbidden.push_back(cp);
    }

    for (int i = 0; i < wantLines.size(); ++i) {
        SCOPED_TRACE("line " + std::to_string(i));
        const QJsonObject want = wantLines[i].toObject();

        bool ok = false;
        const std::vector<int> wantExtracted =
            expectedList(want.value("expected_codepoints").toArray(), &ok);
        ASSERT_TRUE(ok) << "malformed expected_codepoints in sidecar";
        const std::vector<int> wantAuthored =
            expectedList(want.value("authored_codepoints").toArray(), &ok);
        ASSERT_TRUE(ok) << "malformed authored_codepoints in sidecar";

        const std::vector<int> gotCps = codepointsOf(got[i]);

        // (1) Stability: extraction returns exactly the recorded sequence.
        EXPECT_EQ(gotCps, wantExtracted)
            << "extracted  " << describe(gotCps) << "\n"
            << "expected   " << describe(wantExtracted);

        // (2) Correctness: folded, it IS the authored text, in order.
        const std::vector<int> foldedGot =
            applyFolding(gotCps, want.value("presentation_form_folding").toObject());
        EXPECT_EQ(foldedGot, wantAuthored)
            << "extraction does not reproduce the AUTHORED text in logical "
               "order.\n"
            << "  extracted        " << describe(gotCps) << "\n"
            << "  folded           " << describe(foldedGot) << "\n"
            << "  authored (truth) " << describe(wantAuthored) << "\n"
            << "The authored value comes from "
            << root.value("authored_source").toString().toStdString()
            << ", the generator that created this fixture — not from a "
               "recording of extraction output.";

        for (int bad : forbidden) {
            EXPECT_EQ(std::count(gotCps.begin(), gotCps.end(), bad), 0)
                << "extraction emitted forbidden codepoint "
                << describe({bad});
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    I18nRtlCjk, ExtractionFidelitySidecarTest,
    ::testing::Values("i18n_rtl", "i18n_cjk", "i18n_combining"),
    [](const ::testing::TestParamInfo<const char*>& info) {
        return std::string(info.param);
    });

// A sidecar that nothing reads is the very defect PDF-79 names. Assert the set
// of committed sidecars is exactly the set the suite consumes, so adding a
// sidecar without wiring it up is a red test rather than silent dead weight.
TEST(ExtractionFidelitySidecarInventory, EveryCommittedSidecarIsExercised) {
    ASSERT_TRUE(fs::is_directory(sidecarDir())) << sidecarDir();

    std::vector<std::string> onDisk;
    for (const auto& e : fs::directory_iterator(sidecarDir())) {
        const std::string name = e.path().filename().string();
        if (name.size() > 14
            && name.substr(name.size() - 14) == ".expected.json") {
            onDisk.push_back(name.substr(0, name.size() - 14));
        }
    }
    std::sort(onDisk.begin(), onDisk.end());

    const std::vector<std::string> exercised = {
        "i18n_cjk", "i18n_combining", "i18n_rtl"
    };
    EXPECT_EQ(onDisk, exercised)
        << "tests/fixtures/extraction/ holds a sidecar that no test reads (or "
           "is missing one this test expects). Add it to the "
           "INSTANTIATE_TEST_SUITE_P list above.";
}
