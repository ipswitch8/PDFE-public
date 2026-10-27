// -*- coding: utf-8 -*-
//
// Meta-audit follow-up F39-c (`open-backlog-f44-f47` phase-47):
// DECODE-TIME PIXEL-COUNT BOUND on the JPEG2000 (JPX) path.
//
// THE FINDING
// -----------
// A JPX codestream declares its own image size in the SIZ marker, and the PDF
// image dictionary cannot bound it — for JPXDecode, /Width and /Height are
// OPTIONAL (PDF 1.7 §7.4.9) and the codestream wins. The committed fuzz seed
// `tests/security/fuzz/seeds/fuzz_image_decode/seed_jpx_huge_dims.bin` is
// 182 bytes declaring 65535×65535 over an 8×8 payload, and it drove
// `fz_load_jpx` → OpenJPEG to ~2142 MB RSS. Resource exhaustion, NOT memory
// corruption (ASan and UBSan reported nothing), reachable only when the #50
// opt-in JPX gate is ON — the shipped default is OFF.
//
// THE FIX BEING GUARDED HERE
// --------------------------
// `vcpkg-overlay-ports/libmupdf/0001-pdfe-jpx-decode-gate.patch` now also
// defines `FZ_PDFE_JPX_MAX_PIXELS = 600,000,000` and enforces it inside
// `jpx_read_image` immediately after `opj_read_header` and BEFORE
// `opj_decode` — i.e. on the dimensions parsed from the codestream header,
// before OpenJPEG allocates any component buffer. Over-bound declarations are
// refused with `fz_throw(ctx, FZ_ERROR_LIMIT, ...)`, never a C++ throw
// (MuPDF is setjmp/longjmp C; a C++ throw across it is undefined).
//
// WHY 600,000,000 — the arithmetic, re-derived in
// `JpxBoundIsTheDocumentedThresholdAndDiscriminates` below rather than merely
// asserted in prose:
//
//   ISO A0 @ 600 dpi   19866 × 28087 =   557,976,342 px   93.0% of bound  OK
//   ANSI E @ 600 dpi   20400 × 26400 =   538,560,000 px   89.8% of bound  OK
//   ISO A1 @ 600 dpi   14031 × 19866 =   278,739,846 px   46.5% of bound  OK
//   ISO A0 @ 400 dpi   13244 × 18724 =   247,980,656 px   41.3% of bound  OK
//   this file's scan    2480 ×  3508 =     8,699,840 px    1.45% of bound OK
//   the F39-c seed     65535 × 65535 = 4,294,836,225 px   7.16× bound  REFUSED
//
// ISO A0 at 600 dpi is the binding case — the largest sheet in routine
// engineering-drawing / cartographic scanning at the highest optical
// resolution large-format scanners offer — and it fits with 7% to spare.
//
// WHY THIS FILE IS DISCRIMINATING
// -------------------------------
// A guard that refuses everything is not a bound, it is a ban, and it would
// pass a one-sided test just as happily. So the two decode tests sit on
// OPPOSITE sides of the threshold and enter through the SAME function:
// `HugeDeclaredDimensions...` must be REFUSED (and the refusal must happen
// before the allocation — asserted by peak-working-set delta, not just by the
// throw), and `LegitimateA4ScanAt300Dpi...` must SUCCEED, at full declared
// size, with its content intact. Delete the bound and the first goes green-to-
// red; make the bound refuse unconditionally and the second does.
//
// All tests that touch the gate save and restore it, so this file is
// order-independent and repeat-safe, and it never leaves JPX enabled.

#include <gtest/gtest.h>

#include "core/Error.h"
#include "core/JpxSecurityGate.h"
#include "core/MuPdfContext.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {

using pdfe::core::MuPdfContext;

// ---------------------------------------------------------------------------
// The process-start value of the gate flag, captured at DYNAMIC-INIT time —
// i.e. before gtest's main() runs and therefore before any test (in this file
// or any other) has had the chance to toggle it. This is what makes the
// "default is still OFF" assertion below a real check of the compiled-in
// default rather than a check of whatever the last test left behind.
// ---------------------------------------------------------------------------
const int kJpxEnabledAtProcessStart = fz_pdfe_get_jpx_enabled();

// The documented bound. Duplicated here ON PURPOSE as an independent literal:
// the test compares it against the value the LINKED library reports, so a
// silent change to the overlay patch (or a dropped patch) shows up as a
// disagreement instead of two copies of the same mistake agreeing.
constexpr std::int64_t kDocumentedMaxPixels = 600000000;

std::filesystem::path repoRoot()
{
    return std::filesystem::path(PDFE_REPO_ROOT);
}

std::filesystem::path fixturePath(const char* name)
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

std::vector<unsigned char> readAllBytes(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

std::string readAllText(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string s((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
    // core.autocrlf=true: normalise so a content comparison cannot pass in the
    // session that wrote the file and fail after any fresh checkout.
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != '\r') out.push_back(c);
    }
    return out;
}

#ifdef _WIN32
// Peak working set is monotonic for the process, so a DELTA across a decode
// attempt is a sound upper bound on what that attempt allocated and touched --
// PROVIDED the peak is not already far above where we are now. See
// peakDeltaIsEvaluable() below; the sentence this comment used to end with
// ("which the suite does not") was an assumption, and GitHub #246 is what
// happens when it stops holding.
std::uint64_t peakWorkingSetBytes()
{
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return static_cast<std::uint64_t>(pmc.PeakWorkingSetSize);
}

std::uint64_t currentWorkingSetBytes()
{
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return static_cast<std::uint64_t>(pmc.WorkingSetSize);
}
#else
std::uint64_t peakWorkingSetBytes() { return 0; }
std::uint64_t currentWorkingSetBytes() { return 0; }
#endif

// GitHub #246 -- the bound could pass without measuring anything.
//
// `PeakWorkingSetSize` is a MONOTONIC whole-process high-water mark. If a
// neighbouring test in the same process has already pushed the peak well above
// our current footprint, then the allocation this test is looking for can occur
// entirely in that headroom: the peak does not move, `grew` is 0, and
// `EXPECT_LT(grew, 256 MB)` passes -- reporting success for a measurement that
// never happened. That is the C-FAILOPEN shape exactly: the INABILITY to
// evaluate is indistinguishable from a pass, and it is worse here because the
// reading is most likely to be unevaluable in a full-suite run and perfectly
// fine in the isolated re-run someone does to check.
//
// There is no Win32 call that resets the peak, so the fix is not to clear it
// but to REFUSE to draw a conclusion when the headroom could swallow the spike.
// The test asserts evaluability first and fails loudly when it does not hold.
constexpr std::uint64_t kJpxSpikeBytes = 2142ull * 1024 * 1024;  // the measured finding

bool peakDeltaIsEvaluable(std::uint64_t peakBefore, std::uint64_t currentBefore,
                          std::uint64_t spikeBytes = kJpxSpikeBytes)
{
    // Headroom = how much the process could allocate without moving the peak.
    // If that is as large as the spike we are hunting, a zero delta proves
    // nothing.
    if (peakBefore <= currentBefore) return true;   // no headroom at all
    return (peakBefore - currentBefore) < spikeBytes;
}

// RAII gate toggle: enables JPX for the lifetime of the object and restores
// whatever the flag was before, on every exit path.
class ScopedJpxGate {
public:
    explicit ScopedJpxGate(bool enabled)
        : saved_(pdfe::core::isJpxDecodingEnabled())
    {
        pdfe::core::setJpxDecodingEnabled(enabled);
    }
    ~ScopedJpxGate() { pdfe::core::setJpxDecodingEnabled(saved_); }

    ScopedJpxGate(const ScopedJpxGate&) = delete;
    ScopedJpxGate& operator=(const ScopedJpxGate&) = delete;

private:
    bool saved_;
};

// Drive fz_load_jpx directly. Returns true on success and reports the decoded
// geometry plus a couple of sampled gray levels; on a MuPDF error returns false
// and fills `err`. Nothing with a non-trivial destructor lives on the stack
// inside the fz_try scope (MuPdfContext::execute owns that), which is why the
// pixmap is drained into plain scalars here rather than returned.
struct JpxDecodeResult {
    bool ok = false;
    int width = 0;
    int height = 0;
    int components = 0;
    int sampleInBar = -1;     // gray level inside the fixture's dark bar
    int sampleInGradient = -1;// gray level in the smooth gradient below it
    std::string error;
};

JpxDecodeResult decodeJpx(const std::vector<unsigned char>& bytes)
{
    JpxDecodeResult r;
    MuPdfContext ctx;
    int w = 0, h = 0, n = 0, bar = -1, grad = -1;
    try {
        ctx.execute([&](fz_context* c) {
            fz_pixmap* pix = fz_load_jpx(c, bytes.data(), bytes.size(), nullptr);
            if (!pix) return;
            w = pix->w;
            h = pix->h;
            n = pix->n;
            // Sample two known locations of the generated fixture. Guarded so
            // this is harmless for any other input geometry.
            if (w > 1240 && h > 1600 && n >= 1) {
                bar = pix->samples[static_cast<size_t>(300) * pix->stride
                                   + static_cast<size_t>(1240) * n];
                grad = pix->samples[static_cast<size_t>(1500) * pix->stride
                                    + static_cast<size_t>(1240) * n];
            }
            fz_drop_pixmap(c, pix);
        });
        r.ok = true;
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = e.what();
    }
    r.width = w;
    r.height = h;
    r.components = n;
    r.sampleInBar = bar;
    r.sampleInGradient = grad;
    return r;
}

} // namespace

// ===========================================================================
// Criterion 2 — the #50 default must be UNCHANGED by this phase.
// ===========================================================================

// Adding a bound to the decode path must not be mistakable for having widened
// exposure. Two independent readings of the default: the flag as the linked
// library initialises it, and the tracked patch text that produces that
// initialiser. Either alone could drift without the other noticing — the flag
// alone would go stale until the port is rebuilt, the text alone proves nothing
// about the binary in use.
TEST(F39cJpxPixelBound, GateStillDefaultsOffAndTheTrackedPatchStillSaysSo)
{
    EXPECT_EQ(0, kJpxEnabledAtProcessStart)
        << "JPX decoding must still be OFF by default (#50). Captured at "
           "dynamic-init time, before any test could have toggled it.";

    const auto patch = repoRoot()
        / "vcpkg-overlay-ports" / "libmupdf" / "0001-pdfe-jpx-decode-gate.patch";
    ASSERT_TRUE(std::filesystem::exists(patch)) << patch.string();
    const std::string text = readAllText(patch);
    ASSERT_FALSE(text.empty());

    EXPECT_NE(std::string::npos,
              text.find("+static int fz_pdfe_jpx_enabled_flag = 0;"))
        << "the overlay patch must still initialise the #50 gate flag to 0 "
           "(OFF). F39-c adds a bound to the decode path; it does not open it.";
    EXPECT_EQ(std::string::npos, text.find("fz_pdfe_jpx_enabled_flag = 1;"))
        << "nothing in the patch may set the gate on";
}

// ===========================================================================
// Criterion 1 — the bound exists, has the documented value, and the value
// discriminates: it admits every legitimate large-format scan considered when
// it was chosen, and refuses the F39-c declaration.
// ===========================================================================

TEST(F39cJpxPixelBound, BoundIsTheDocumentedThresholdAndDiscriminates)
{
    const std::int64_t bound = pdfe::core::jpxMaxDecodePixels();

    ASSERT_EQ(kDocumentedMaxPixels, bound)
        << "the linked MuPDF's FZ_PDFE_JPX_MAX_PIXELS disagrees with the "
           "documented threshold. Either the overlay patch was not rebuilt, or "
           "the bound changed without the rationale being revisited.";

    // Legitimate large-format / high-DPI scans — all must be ADMITTED.
    // Dimensions derived from the physical sheet size, not copied from prose.
    struct Case { const char* what; std::int64_t w; std::int64_t h; };
    const Case admitted[] = {
        { "ISO A0 @ 600 dpi (841x1189 mm) - the binding case", 19866, 28087 },
        { "ANSI E @ 600 dpi (34x44 in)",                       20400, 26400 },
        { "ISO A1 @ 600 dpi (594x841 mm)",                     14031, 19866 },
        { "ISO A0 @ 400 dpi",                                  13244, 18724 },
        { "35 mm film @ 4000 dpi",                              5669,  3780 },
        { "this file's committed A4 @ 300 dpi fixture",         2480,  3508 },
    };
    for (const auto& c : admitted) {
        const std::int64_t px = c.w * c.h;
        EXPECT_LE(px, bound)
            << c.what << " is " << c.w << "x" << c.h << " = " << px
            << " px and MUST NOT be refused — the bound would be excluding "
               "real documents.";
    }

    // The A0-at-600-dpi headroom is the whole justification for the value; if
    // it ever became negative or absurdly large the rationale is stale.
    const std::int64_t a0at600 = std::int64_t(19866) * 28087;
    EXPECT_EQ(557976342, a0at600);
    EXPECT_GT(bound, a0at600) << "bound must admit the binding case";
    EXPECT_LT(bound, a0at600 * 2)
        << "bound has drifted far above the case it was justified by; the "
           "written rationale no longer explains it";

    // The finding — must be REFUSED, with real margin.
    const std::int64_t attack = std::int64_t(65535) * 65535;
    EXPECT_EQ(4294836225LL, attack);
    EXPECT_GT(attack, bound * 7)
        << "the F39-c declaration must be far over the bound, not marginal";

    // Out-of-class cases the bound deliberately refuses, recorded so that a
    // later widening has to argue with a test rather than with a comment.
    EXPECT_GT(std::int64_t(39732) * 56173, bound)   // ISO A0 @ 1200 dpi
        << "A0 @ 1200 dpi is documented as refused";
    EXPECT_GT(std::int64_t(32000) * 40000, bound)   // 8x10 in @ 4000 dpi
        << "an 8x10 in drum scan @ 4000 dpi is documented as refused";
}

// The bound is only useful where it is placed. Enforced after opj_decode it
// would be a report, not a defence: the allocation would already have happened.
// This reads the tracked patch and asserts the check precedes the decode call.
TEST(F39cJpxPixelBound, BoundIsEnforcedBeforeTheOpenJpegDecodeCall)
{
    const auto patch = repoRoot()
        / "vcpkg-overlay-ports" / "libmupdf" / "0001-pdfe-jpx-decode-gate.patch";
    const std::string text = readAllText(patch);
    ASSERT_FALSE(text.empty());

    // Anchored on the read-header FAILURE branch rather than the call itself:
    // in a unified diff only the lines inside a hunk's context window appear,
    // and `opj_read_header(...)` is four lines above the insertion point, i.e.
    // outside it. This string is the last line of that branch and is the
    // nearest committed evidence that the insertion follows header parsing.
    const auto header = text.find("Failed to read JPX header");
    const auto check  = text.find("jw * jh > FZ_PDFE_JPX_MAX_PIXELS");
    const auto decode = text.find("opj_decode(codec, stream, jpx)");

    ASSERT_NE(std::string::npos, header) << "patch lost its read-header context";
    ASSERT_NE(std::string::npos, check)  << "patch lost the pixel-count check";
    ASSERT_NE(std::string::npos, decode) << "patch lost its decode context";

    EXPECT_LT(header, check)
        << "the bound must be applied to dimensions PARSED FROM THE HEADER";
    EXPECT_LT(check, decode)
        << "the bound must be applied BEFORE opj_decode allocates component "
           "buffers, otherwise the >2 GB allocation still happens";

    // FZ_ERROR_LIMIT, not a C++ throw: MuPDF unwinds with longjmp.
    EXPECT_NE(std::string::npos, text.find("fz_throw(ctx, FZ_ERROR_LIMIT,"))
        << "refusal must use fz_throw with a MuPDF error code";
}

// ===========================================================================
// Criterion 3 — the two-sided decode test. Same entry point, opposite verdicts.
// ===========================================================================

TEST(F39cJpxPixelBound, HugeDeclaredDimensionsAreRefusedBeforeTheAllocation)
{
    const auto seed = repoRoot() / "tests" / "security" / "fuzz" / "seeds"
                      / "fuzz_image_decode" / "seed_jpx_huge_dims.bin";
    ASSERT_TRUE(std::filesystem::exists(seed)) << seed.string();
    const std::vector<unsigned char> bytes = readAllBytes(seed);
    ASSERT_FALSE(bytes.empty());

    ScopedJpxGate gate(true);
    ASSERT_TRUE(pdfe::core::isJpxDecodingEnabled())
        << "without the opt-in gate this would exercise the 1x1 placeholder, "
           "not the decode path, and would prove nothing";

    const std::uint64_t peakBefore = peakWorkingSetBytes();
    const std::uint64_t currentBefore = currentWorkingSetBytes();
    const JpxDecodeResult r = decodeJpx(bytes);
    const std::uint64_t peakAfter = peakWorkingSetBytes();

    EXPECT_FALSE(r.ok)
        << "a JPX declaring 65535x65535 (4,294,836,225 px, 7.16x the bound) "
           "must be refused, not decoded";
    EXPECT_NE(std::string::npos, r.error.find("PDFE decode bound"))
        << "refusal must come from the F39-c bound, not from some other "
           "failure that happens to keep this test green. Got: " << r.error;

#ifdef _WIN32
    ASSERT_GT(peakBefore, 0u);

    // GitHub #246. Establish that a zero delta would MEAN anything before
    // reading one. A neighbour-raised peak leaves headroom the spike could hide
    // in, and then `grew == 0` says nothing at all.
    ASSERT_TRUE(peakDeltaIsEvaluable(peakBefore, currentBefore))
        << "peak working set is " << (peakBefore / (1024 * 1024))
        << " MB against a current " << (currentBefore / (1024 * 1024))
        << " MB, leaving " << ((peakBefore - currentBefore) / (1024 * 1024))
        << " MB of headroom -- at least as much as the "
        << (kJpxSpikeBytes / (1024 * 1024))
        << " MB allocation this test exists to detect. A zero delta here would "
           "be unevaluable, not a pass. Run this test in a fresh process "
           "(--gtest_filter) rather than trusting the reading.";

    const std::uint64_t grew = (peakAfter > peakBefore) ? (peakAfter - peakBefore) : 0;
    // The finding measured 2142 MB. Refusing before opj_decode means no
    // component buffer is allocated at all, so the attempt should cost
    // essentially nothing; 256 MB is a generous ceiling that still fails by an
    // order of magnitude if the allocation is reinstated.
    EXPECT_LT(grew, std::uint64_t(256) * 1024 * 1024)
        << "peak working set grew by " << (grew / (1024 * 1024))
        << " MB across the refused decode — the bound is not firing before the "
           "allocation (the finding measured 2142 MB)";
#endif
}

TEST(F39cJpxPixelBound, LegitimateA4ScanAt300DpiStillDecodesInFull)
{
    const auto fixture = fixturePath("jpx_legit_a4_300dpi.jp2");
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << fixture.string()
        << " — regenerate with tests/fixtures/gen_jpx_legit_scan.py";
    const std::vector<unsigned char> bytes = readAllBytes(fixture);
    ASSERT_FALSE(bytes.empty());

    // Sanity: this fixture is on the ADMITTED side of the threshold.
    ASSERT_LT(std::int64_t(2480) * 3508, pdfe::core::jpxMaxDecodePixels());

    ScopedJpxGate gate(true);
    ASSERT_TRUE(pdfe::core::isJpxDecodingEnabled());

    const JpxDecodeResult r = decodeJpx(bytes);

    ASSERT_TRUE(r.ok)
        << "an ISO A4 @ 300 dpi scan (8,699,840 px, 1.45% of the bound) must "
           "still decode — a bound that refuses this is a ban, not a bound. "
           "Error: " << r.error;
    EXPECT_EQ(2480, r.width)  << "must decode at full declared width";
    EXPECT_EQ(3508, r.height) << "must decode at full declared height";
    EXPECT_EQ(1, r.components) << "single-component grayscale scan";

    // Content check: the generator paints a dark bar (level 16) over rows
    // 200..400 and a smooth mid-gradient elsewhere. Without this, a decode that
    // returned a correctly-sized but blank pixmap would pass. Lossy (9/7
    // irreversible) coding shifts levels slightly, hence the tolerance.
    ASSERT_GE(r.sampleInBar, 0);
    ASSERT_GE(r.sampleInGradient, 0);
    EXPECT_LT(r.sampleInBar, 60)
        << "the dark bar decoded as " << r.sampleInBar
        << "; the pixmap is not the fixture's content";
    EXPECT_GT(r.sampleInGradient, r.sampleInBar + 40)
        << "gradient (" << r.sampleInGradient << ") must be clearly lighter "
           "than the bar (" << r.sampleInBar << ")";
}

// Belt and braces: the gate is left OFF after this file's tests, so nothing
// downstream inherits an opted-in decoder.
TEST(F39cJpxPixelBound, ZzGateIsRestoredOffAfterThisFile)
{
    EXPECT_FALSE(pdfe::core::isJpxDecodingEnabled())
        << "a test in this file leaked an enabled JPX gate into the rest of "
           "the suite";
}

// ---------------------------------------------------------------------------
// GitHub #246 -- the peak-delta bound could report success without measuring.
// ---------------------------------------------------------------------------
TEST(F39cJpxPixelBound, AZeroPeakDeltaIsOnlyMeaningfulWithoutHeadroom)
{
    constexpr std::uint64_t MB = 1024ull * 1024;

    // A fresh process: the peak IS the current footprint, so any allocation
    // must move it. A zero delta here genuinely means nothing was allocated.
    EXPECT_TRUE(peakDeltaIsEvaluable(300 * MB, 300 * MB));

    // Modest headroom, far smaller than the 2142 MB spike: still evaluable,
    // because the spike could not fit inside it.
    EXPECT_TRUE(peakDeltaIsEvaluable(500 * MB, 300 * MB));

    // A neighbouring test pushed the peak 3.2 GB above our current footprint.
    // The whole 2142 MB allocation now fits in the headroom without moving the
    // peak, so `grew == 0` proves nothing -- this is the #246 false pass, and
    // the bound must refuse rather than report success.
    EXPECT_FALSE(peakDeltaIsEvaluable(3500 * MB, 300 * MB));

    // Exactly at the boundary: headroom equal to the spike is NOT evaluable.
    EXPECT_FALSE(peakDeltaIsEvaluable(300 * MB + kJpxSpikeBytes, 300 * MB));

    // One below the boundary is.
    EXPECT_TRUE(peakDeltaIsEvaluable(300 * MB + kJpxSpikeBytes - 1, 300 * MB));
}

TEST(F39cJpxPixelBound, TheBoundActuallyConsultsTheEvaluabilityCheck)
{
    // A text-over-source oracle, and deliberately so: the predicate above can
    // be correct while nothing calls it, which is how #246 would come back.
    // This is the cheapest thing that fails when the guard is unwired; it is
    // not a substitute for the predicate cases above, it is the wiring check.
    const auto self = repoRoot() / "tests" / "test_f39c_jpx_pixel_bound.cpp";
    const std::string body = readAllText(self);
    ASSERT_FALSE(body.empty()) << "cannot read " << self;

    const auto guard = body.find("ASSERT_TRUE(peakDeltaIsEvaluable(");
    ASSERT_NE(guard, std::string::npos)
        << "the refused-decode bound no longer asserts that its peak-delta "
           "reading is evaluable; a neighbour-raised process peak can make it "
           "pass while measuring nothing (GitHub #246)";

    // The CODE form, not the prose form. Searching for "EXPECT_LT(grew," alone
    // matched the explanatory comment above -- which sits BEFORE the guard, so
    // this test failed on its own documentation. Same comment-versus-code
    // confusion as DCA 0094, in the test written to catch unwiring.
    const auto reading = body.find("EXPECT_LT(grew, std::uint64_t(");
    ASSERT_NE(reading, std::string::npos) << "the peak-delta bound is gone";
    EXPECT_LT(guard, reading)
        << "the evaluability assertion must come BEFORE the delta is read";
}
