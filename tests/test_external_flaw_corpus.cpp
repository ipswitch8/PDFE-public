// -*- coding: utf-8 -*-
//
// External flaw corpus — targeted regression tests for PDF flaw classes that
// the existing PDFE corpus did NOT cover, sourced from documented upstream
// suites (mozilla/pdf.js, itext7) per pdf-test-corpus-reference.md.
//
// Fixtures live under tests/fixtures/flaw_corpus/<category>/ and are the real,
// upstream-documented files. Each test asserts the behaviour that was verified
// against the same MuPDF core PDFE ships (via PyMuPDF 1.26): render/extract
// succeeds on the well-formed-but-tricky cases, the type-3 recursion is BOUNDED
// (MuPDF detects "recursive type3 font" and returns rather than looping), and
// certificate (public-key) encryption — which MuPDF cannot open — fails
// GRACEFULLY (clean throw or null) instead of crashing.
//
// Contract: open/render/extract must not CRASH or HANG on any of these; where a
// feature is genuinely unsupported (cert encryption), the failure must be clean.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/JpxSecurityGate.h"
#include "core/Jbig2SecurityGate.h"
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "ui/PdfCanvas.h"
#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"
#include "security_oracles.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path ext_fixture(const char* rel) {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "flaw_corpus" / rel;
}

// ── Glyph-cache accounting seam ─────────────────────────────────────────────
//
// MuPDF DOES expose glyph-cache accounting: `fz_dump_glyph_cache_stats` is
// declared in mupdf/fitz/glyph-cache.h and is a defined external symbol in
// libmupdf. It writes free-form debug text to an fz_output; we render that into
// an in-memory fz_buffer and parse the numbers back out.
//
// setjmp/longjmp discipline (GitHub #118): the capture buffer is a raw char
// array — trivially destructible — so a longjmp out of the fz_try cannot skip a
// destructor. Nothing with a non-trivial destructor is constructed inside.
constexpr size_t kGlyphDumpMax = 8192;

// Returns the raw dump text for `c`'s glyph cache, or "" if the dump threw.
std::string glyphCacheDump(fz_context* c) {
    char dump[kGlyphDumpMax] = {0};
    fz_buffer* buf = nullptr;
    fz_output* out = nullptr;
    fz_try(c) {
        buf = fz_new_buffer(c, kGlyphDumpMax);
        out = fz_new_output_with_buffer(c, buf);
        fz_dump_glyph_cache_stats(c, out);
        fz_close_output(c, out);
        const char* s = fz_string_from_buffer(c, buf);
        if (s != nullptr) {
            std::strncpy(dump, s, sizeof(dump) - 1);
        }
    }
    fz_always(c) {
        if (out != nullptr) fz_drop_output(c, out);
        if (buf != nullptr) fz_drop_buffer(c, buf);
    }
    fz_catch(c) {
        dump[0] = '\0';
    }
    return std::string(dump);
}

// Open `path` on context `renderCtx`, rasterize page 0, and return the glyph-
// cache dump read from `readCtx` — taken BEFORE the document is dropped.
//
// The ordering is load-bearing and was established empirically, not assumed:
// MuPDF's `pdf_drop_document_imp` (source/pdf/pdf-xref.c) calls
// `fz_purge_glyph_cache` unconditionally, because cached Type 3 glyphs hold
// pdf_obj pointers into the document being destroyed. Reading the dump after
// the close therefore ALWAYS yields "Glyph Cache Size: 0" — the first version
// of this helper did exactly that, and every measurement was a vacuous zero.
//
// Rasterizing — not merely building a display list — is what forces glyphs
// through the cache. Render failures are swallowed: a fixture that cannot
// render still counts as bounded, and the caller's vacuity guards catch a
// measurement that came back empty.
std::string dumpAfterRasterizingPageZero(fz_context* renderCtx,
                                         fz_context* readCtx,
                                         const char* path) {
    char dump[kGlyphDumpMax] = {0};
    fz_document* d = nullptr;
    fz_pixmap* pix = nullptr;
    fz_try(renderCtx) {
        d = fz_open_document(renderCtx, path);
        pix = fz_new_pixmap_from_page_number(renderCtx, d, 0, fz_identity,
                                             fz_device_rgb(renderCtx), 0);
    }
    fz_catch(renderCtx) {
        // bounded failure is acceptable here
    }
    // Read the cache while the document — and therefore the cache — still lives.
    const std::string live = glyphCacheDump(readCtx);
    std::strncpy(dump, live.c_str(), sizeof(dump) - 1);
    fz_try(renderCtx) {
        if (pix != nullptr) fz_drop_pixmap(renderCtx, pix);
        if (d != nullptr) fz_drop_document(renderCtx, d);
    }
    fz_catch(renderCtx) {
    }
    return std::string(dump);
}

// Parse "Glyph Cache Size: <n>" out of a dump. Returns false if the line (or a
// number on it) is absent — the caller MUST treat that as a failure rather than
// as a population of zero, or the whole measurement silently becomes vacuous.
bool parseGlyphCacheSize(const std::string& dump, unsigned long long* out) {
    const std::string key = "Glyph Cache Size:";
    const auto pos = dump.find(key);
    if (pos == std::string::npos) return false;
    const char* p = dump.c_str() + pos + key.size();
    while (*p == ' ' || *p == '\t') ++p;
    if (*p < '0' || *p > '9') return false;
    unsigned long long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10ULL + static_cast<unsigned long long>(*p - '0');
        ++p;
    }
    *out = v;
    return true;
}

// Parse "Glyph Cache Evictions: <n> (<m> bytes)" out of a dump — the count, not
// the byte figure. Returns false if the line (or a number on it) is absent.
//
// THAT CASE IS REAL AND MUST NOT BE FAKED AS ZERO: MuPDF only emits this line
// from a libmupdf built without NDEBUG (the eviction counters are themselves
// debug-only), so a Release libmupdf produces a dump with the size line and no
// eviction line. A caller that read "absent" as "zero evictions" could not
// distinguish "looked and found none" from "did not look", which is exactly the
// failure this parser's sibling was written to avoid. The caller must SKIP the
// eviction guard and say so, not pass it.
bool parseGlyphCacheEvictions(const std::string& dump, unsigned long long* out) {
    const std::string key = "Glyph Cache Evictions:";
    const auto pos = dump.find(key);
    if (pos == std::string::npos) return false;
    const char* p = dump.c_str() + pos + key.size();
    while (*p == ' ' || *p == '\t') ++p;
    if (*p < '0' || *p > '9') return false;
    unsigned long long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10ULL + static_cast<unsigned long long>(*p - '0');
        ++p;
    }
    *out = v;
    return true;
}

class ExternalFlawCorpusTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
    }
    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;

    // Open a fixture, skipping cleanly if it was not downloaded.
    std::unique_ptr<pdfe::core::Document> openOrSkip(const char* rel) {
        auto path = ext_fixture(rel);
        if (!std::filesystem::exists(path)) {
            ADD_FAILURE() << "external fixture missing (run the corpus "
                             "downloader): " << path.string();
            return nullptr;
        }
        return pdfe::core::Document::open(ctx_, path);
    }

    // ── GitHub #53: real RASTERIZATION, with a clock ───────────────────────
    //
    // renderPageZero() below builds a display list and drops it. That is
    // sufficient for the type-3 recursion (which MuPDF refuses while BUILDING
    // the list) but it is NOT sufficient for a recursive tiling PATTERN: a
    // display list records the fill operation, and the pattern painter only
    // descends when the list is RASTERIZED. A self-referential tiling pattern
    // therefore passes a display-list-only check while still being able to
    // exhaust the stack on the first real paint.
    //
    // So this helper goes all the way to a pixmap, and reports the three things
    // a bound needs: did it come back, did it produce anything, and how long it
    // took.
    struct RasterOutcome {
        bool opened = false;        // fz_open_document returned a document
        bool rendered = false;      // a pixmap was produced
        bool boundedError = false;  // MuPDF REFUSED it -- and returned, which is
                                    // the outcome this test wants to distinguish
                                    // from "never came back at all"
        long long pixels = 0;
        long long elapsedMs = 0;
    };

    RasterOutcome rasterizePageZero(const char* rel) {
        RasterOutcome out;
        const std::string pathStr = ext_fixture(rel).string();
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        const auto t0 = std::chrono::steady_clock::now();
        // No object with a non-trivial destructor inside the lambda (#118), and
        // no C++ exception may cross the fz_try frame.
        ctx->execute([&out, &pathStr](fz_context* c) {
            fz_document* d = nullptr;
            fz_pixmap* pix = nullptr;
            fz_var(d);
            fz_var(pix);
            fz_try(c) {
                d = fz_open_document(c, pathStr.c_str());
                out.opened = (d != nullptr);
                pix = fz_new_pixmap_from_page_number(c, d, 0, fz_identity,
                                                     fz_device_rgb(c), 0);
                if (pix != nullptr) {
                    out.rendered = true;
                    out.pixels = static_cast<long long>(fz_pixmap_width(c, pix)) *
                                 static_cast<long long>(fz_pixmap_height(c, pix));
                }
            }
            fz_always(c) {
                if (pix != nullptr) fz_drop_pixmap(c, pix);
                if (d != nullptr) fz_drop_document(c, d);
            }
            fz_catch(c) {
                out.boundedError = true;
            }
        });
        out.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        return out;
    }

    // GitHub #53 NON-VACUITY: prove the fixture really is self-referential at
    // the OBJECT level. Returns true when the page's /Pattern /P1 resolves to an
    // object whose OWN /Resources /Pattern /P1 is that same object number.
    //
    // Without this, a fixture that was regenerated wrong -- or silently replaced
    // by an ordinary, non-recursive tiling pattern -- would sail through the
    // bound below, because a pattern that does not recurse is trivially bounded.
    // The bound would then be measuring nothing, which is the exact failure mode
    // this corpus keeps finding.
    bool patternSelfReferences(const char* rel) {
        const std::string pathStr = ext_fixture(rel).string();
        bool selfRef = false;
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        ctx->execute([&selfRef, &pathStr](fz_context* c) {
            fz_document* d = nullptr;
            fz_var(d);
            fz_try(c) {
                d = fz_open_document(c, pathStr.c_str());
                pdf_document* pd = pdf_specifics(c, d);
                if (pd != nullptr) {
                    pdf_obj* page = pdf_lookup_page_obj(c, pd, 0);
                    pdf_obj* res = page ? pdf_dict_get(c, page, PDF_NAME(Resources)) : nullptr;
                    pdf_obj* pats = res ? pdf_dict_get(c, res, PDF_NAME(Pattern)) : nullptr;
                    // The page's pattern entry, kept as the INDIRECT reference
                    // so its object number can be compared.
                    pdf_obj* p1 = pats ? pdf_dict_gets(c, pats, "P1") : nullptr;
                    if (p1 != nullptr && pdf_is_indirect(c, p1)) {
                        const int outerNum = pdf_to_num(c, p1);
                        pdf_obj* pat = pdf_resolve_indirect(c, p1);
                        pdf_obj* pres = pat ? pdf_dict_get(c, pat, PDF_NAME(Resources)) : nullptr;
                        pdf_obj* ppats = pres ? pdf_dict_get(c, pres, PDF_NAME(Pattern)) : nullptr;
                        pdf_obj* inner = ppats ? pdf_dict_gets(c, ppats, "P1") : nullptr;
                        if (inner != nullptr && pdf_is_indirect(c, inner)) {
                            selfRef = (pdf_to_num(c, inner) == outerNum);
                        }
                    }
                }
            }
            fz_always(c) {
                if (d != nullptr) fz_drop_document(c, d);
            }
            fz_catch(c) {
                // leave selfRef false; the caller asserts on it
            }
        });
        return selfRef;
    }

    // Build + drop the page-0 display list. Returns true if a display list was
    // produced. Merely COMPLETING this call is the bound-check for the
    // recursive-type3 case: an unbounded interpreter would never return.
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

    // ── SEC-84 / GitHub #65 ─────────────────────────────────
    //
    // The /Width and /Height the XObject DICTIONARY declares, read straight from
    // the PDF object graph.
    //
    // Deliberately NOT from ImageManager::enumerateImages: measured, its
    // `pixelWidth`/`pixelHeight` report the DECODED size, so for a gated JPX
    // image they are the 1x1 placeholder -- comparing that against the decoded
    // pixmap would be 1-vs-1 and prove nothing. (A vacuity guard in the test
    // below caught exactly that before it could pass.) The dictionary is the
    // one source the gate does not touch.
    //
    // Returns {0,0} if no image XObject with both keys is found.
    std::pair<int, int> declaredImageSize(const char* rel) {
        const auto path = ext_fixture(rel);
        const std::string pathStr = path.string();
        int w = 0, h = 0;
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        // No object with a non-trivial destructor inside the lambda (#118).
        ctx->execute([&w, &h, &pathStr](fz_context* c) {
            fz_document* d = fz_open_document(c, pathStr.c_str());
            if (d == nullptr) return;
            fz_try(c) {
                pdf_document* pd = pdf_specifics(c, d);
                if (pd != nullptr) {
                    const int n = pdf_count_objects(c, pd);
                    for (int i = 1; i < n && w == 0; ++i) {
                        pdf_obj* o = pdf_load_object(c, pd, i);
                        if (o == nullptr) continue;
                        pdf_obj* sub = pdf_dict_get(c, o, PDF_NAME(Subtype));
                        if (pdf_name_eq(c, sub, PDF_NAME(Image))) {
                            const int ow = pdf_dict_get_int(c, o, PDF_NAME(Width));
                            const int oh = pdf_dict_get_int(c, o, PDF_NAME(Height));
                            if (ow > 0 && oh > 0) { w = ow; h = oh; }
                        }
                        pdf_drop_obj(c, o);
                    }
                }
            } fz_always(c) {
                fz_drop_document(c, d);
            } fz_catch(c) {
                fz_rethrow(c);
            }
        });
        return {w, h};
    }

    // ── GitHub #126 ─────────────────────────────────────────────────────────
    //
    // Did MuPDF have to RECONSTRUCT the xref to open this file?
    //
    // `COVERAGE.md` describes the seven Corrupt_* fixtures as the
    // corrupt/recovery class. Mutation evidence
    // (docs/audit/phase4-residuals-phase2-mutation-evidence-2026-09-01.md)
    // showed only FOUR of them actually force a rebuild; the other three are
    // malformed in ways MuPDF parses without one. That is fine -- they are
    // still worth having -- but a reader would reasonably conclude all seven
    // cover xref reconstruction, and a regression in the rebuild path would be
    // caught by four tests while the docs implied seven.
    //
    // `pdf_was_repaired()` is the seam that answers it, so the distinction is
    // asserted here rather than described in a markdown file. BOTH directions
    // are pinned: a fixture that stops repairing reddens, and so does one that
    // starts.
    //
    // Opens the file independently of `Document`, whose `fzDocument()` is
    // private. NOTE the lambda body holds no object with a non-trivial
    // destructor: `execute` runs inside MuPDF's setjmp/longjmp, so an fz_throw
    // would skip any such destructor (GitHub #118, and the class this repo
    // spent phase-10 burning down). The path string is built OUTSIDE.
    bool repairPathTaken(const char* rel) {
        const auto path = ext_fixture(rel);
        const std::string pathStr = path.string();
        bool repaired = false;
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        ctx->execute([&repaired, &pathStr](fz_context* c) {
            fz_document* d = fz_open_document(c, pathStr.c_str());
            if (d == nullptr) return;
            fz_try(c) {
                pdf_document* pd = pdf_specifics(c, d);
                if (pd != nullptr) {
                    repaired = pdf_was_repaired(c, pd) != 0;
                }
            } fz_always(c) {
                fz_drop_document(c, d);
            } fz_catch(c) {
                fz_rethrow(c);
            }
        });
        return repaired;
    }

    // ── PDF-87 (meta-audit gap register, issue #34) ─────────────────────────
    //
    // The register asks for the Corrupt_* family to be wrapped in a shared
    // `EXPECT_LT(elapsed, 5s)` helper. That specific form was declined once and
    // is still declined: an absolute wall-clock threshold on a shared, loaded
    // machine reddens at random, which trains everyone to ignore the colour.
    // Same class as PDF-83's peak-memory bound.
    //
    // What IS worth guarding is the property behind it — a corrupt file must
    // not send the parser into pathological work. This measures the corrupt
    // fixture against a WELL-FORMED CONTROL parsed by the same code path in the
    // same process, best-of-N on both sides, and bounds the RATIO. Machine
    // speed, CPU contention, Debug-build overhead and antivirus interference
    // all move both measurements together and cancel out; an absolute
    // millisecond budget cancels nothing.
    //
    // WHAT THIS DOES AND DOES NOT CATCH, stated plainly rather than overclaimed:
    // a genuinely infinite loop hangs the process and no in-process assertion
    // can catch it — the CI job timeout is the only backstop for that. What
    // this catches is the realistic and far more common failure: parsing that
    // is finite but pathological — a quadratic xref rebuild, a retry storm, a
    // recovery path that rescans the file per object. Those are exactly the
    // regressions a corrupt-file corpus exists to notice, and nothing here
    // noticed them before.
    //
    // The ratio is deliberately loose. It is a blowup detector, not a
    // performance budget: pathological parsing is orders of magnitude off, not
    // 20% off, and a tight bound here would be the flaky test this design
    // exists to avoid. kFloorMs keeps a sub-millisecond control from making the
    // bound absurd.
    static constexpr double kMaxRatioVsControl = 40.0;
    static constexpr double kFloorMs = 750.0;

    // Best-of-N wall time (ms) for open + page-0 render of `path`. Best-of-N,
    // not mean: a scheduler stall inflates a sample but cannot deflate one, so
    // the minimum is the closest available estimate of the real work done.
    double bestOfOpenAndRender(const std::filesystem::path& path, int n = 3) {
        double best = -1.0;
        for (int i = 0; i < n; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            {
                auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
                auto doc = pdfe::core::Document::open(ctx, path);
                if (doc && doc->pageCount() >= 1) {
                    void* dl = doc->createDisplayList(0);
                    if (dl) {
                        doc->context()->execute([dl](fz_context* c) {
                            fz_drop_display_list(c,
                                static_cast<fz_display_list*>(dl));
                        });
                    }
                }
            }
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            if (best < 0.0 || ms < best) best = ms;
        }
        return best;
    }

    // The shared guard the register asked for, in a form that cannot redden at
    // random. Called by every Corrupt_* test.
    void expectBoundedAgainstControl(const char* rel) {
        const auto corrupt = ext_fixture(rel);
        const auto control =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        ASSERT_TRUE(std::filesystem::exists(control))
            << "the well-formed control fixture is missing, so there is nothing "
               "to normalise against and this guard would silently become an "
               "absolute threshold: " << control.string();
        ASSERT_TRUE(std::filesystem::exists(corrupt)) << corrupt.string();

        const double controlMs = bestOfOpenAndRender(control);
        const double corruptMs = bestOfOpenAndRender(corrupt);
        ASSERT_GT(controlMs, 0.0)
            << "the control measured as zero work — the ratio below would be "
               "meaningless";

        const double budget = controlMs * kMaxRatioVsControl + kFloorMs;
        EXPECT_LE(corruptMs, budget)
            << "parsing the corrupt fixture " << rel << " took " << corruptMs
            << " ms against a " << controlMs << " ms well-formed control in the "
            << "same process (ratio " << (corruptMs / controlMs) << "x, budget "
            << budget << " ms). That is pathological work on a damaged file — "
               "the shape of a quadratic rebuild or a retry storm, not of a "
               "slow machine, because the control absorbed the machine.";
    }

    // ── Phase-12 AC4, second half: GLYPH-CACHE POPULATION bound ──────────────
    //
    // The wall-clock half (above) bounds how LONG the recursive Type 3 fixture
    // takes. It says nothing about how much the recursion made MuPDF CACHE. A
    // Type 3 glyph whose content stream re-enters itself is exactly the shape
    // that inflates glyph-cache population without necessarily inflating wall
    // clock much, so the two halves catch different regressions.
    //
    // The seam is `fz_dump_glyph_cache_stats` (mupdf/fitz/glyph-cache.h). It
    // was once claimed MuPDF "exposes no public accounting of glyph-cache
    // population"; that is wrong — the function is declared in the public
    // header and defined in libmupdf, and it reports
    // `fz_glyph_cache::total`, the live byte count of cached glyphs.
    //
    // Bounded against the CONTROL document, not an absolute byte count, for
    // the same reason as the wall-clock half: an absolute threshold reddens on
    // a different machine or MuPDF build.
    //
    // STATED LIMITATION, and now CHECKED rather than estimated: MuPDF evicts
    // once its glyph cache passes an internal ceiling, and past that point
    // `Glyph Cache Size` no longer reflects how much the document TRIED to
    // cache — the ratio stops measuring what it claims and no budget above the
    // ceiling could ever redden. An earlier version of this comment named a
    // specific constant and derived a saturation ratio from it; the two figures
    // were mutually inconsistent and neither was verified from source, so both
    // are gone. Saturation is instead ASSERTED AGAINST DIRECTLY via the dump's
    // eviction count (vacuity guard 4 below): zero evictions means the reading
    // never hit the ceiling, whatever the ceiling happens to be on this build.
    //
    // Observed on this build, 2026-09-07: control `test_3page.pdf` = 22,666
    // bytes, recursive Type 3 fixture = 72,553 bytes, ratio 3.20x, budget
    // `Pc * 8.0 + 64 KiB` = 246,864 bytes, evictions 0 on both sides.
    static constexpr double kMaxGlyphCacheRatioVsControl = 8.0;
    static constexpr unsigned long long kGlyphCacheFloorBytes = 64ULL * 1024ULL;

    // Purge, rasterize `path` on the MAIN context, and return the live cached
    // byte count. Sets `*ok` false if the dump was empty or unparseable — a
    // caller that ignored that would silently read every measurement as 0 and
    // pass unconditionally.
    unsigned long long glyphCachePopulationFor(const std::string& path,
                                               bool* ok, std::string* rawOut,
                                               bool* evictionsKnown = nullptr,
                                               unsigned long long* evictions =
                                                   nullptr) {
        fz_context* c = ctx_->get();
        fz_purge_glyph_cache(c);
        const std::string raw = dumpAfterRasterizingPageZero(c, c, path.c_str());
        if (rawOut != nullptr) *rawOut = raw;
        unsigned long long v = 0;
        const bool parsed = !raw.empty() && parseGlyphCacheSize(raw, &v);
        *ok = parsed;
        if (evictionsKnown != nullptr) {
            unsigned long long e = 0;
            // Deliberately NOT defaulted to 0 on absence — see
            // parseGlyphCacheEvictions. `*evictionsKnown == false` means the
            // build did not report evictions at all.
            *evictionsKnown = !raw.empty() && parseGlyphCacheEvictions(raw, &e);
            if (evictions != nullptr && *evictionsKnown) *evictions = e;
        }
        return v;
    }

    void expectGlyphCacheBoundedAgainstControl(const char* rel) {
        const auto fixture = ext_fixture(rel);
        const auto control =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        ASSERT_TRUE(std::filesystem::exists(control)) << control.string();
        ASSERT_TRUE(std::filesystem::exists(fixture)) << fixture.string();
        const std::string controlStr = control.string();
        const std::string fixtureStr = fixture.string();

        // ── VACUITY GUARD 1 — the cache is genuinely SHARED with the thread
        // clone that Document's render path uses.
        //
        // MuPdfContext::currentThreadClone() means a render can run on a
        // cloned fz_context. If fz_clone_context did not share the glyph-cache
        // context, a reading taken from get() would be a constant zero and
        // every assertion below would be decoration. Proven by moving it:
        // purge, read (must be 0), render on the CLONE, read from MAIN again.
        {
            fz_context* main = ctx_->get();
            fz_purge_glyph_cache(main);
            unsigned long long before = 0;
            const std::string beforeRaw = glyphCacheDump(main);
            ASSERT_TRUE(parseGlyphCacheSize(beforeRaw, &before))
                << "fz_dump_glyph_cache_stats produced nothing parseable, so "
                   "the glyph-cache metric cannot be read at all. Dump was: ["
                << beforeRaw << "]";
            ASSERT_EQ(before, 0ULL)
                << "the cache did not purge, so no measurement below starts "
                   "from a known state";
            fz_context* clone = ctx_->threadLocalClone();
            ASSERT_NE(clone, main) << "threadLocalClone() handed back the main "
                                      "context, so this proves nothing";
            const std::string afterRaw =
                dumpAfterRasterizingPageZero(clone, main, controlStr.c_str());
            unsigned long long after = 0;
            ASSERT_TRUE(parseGlyphCacheSize(afterRaw, &after))
                << "dump unparseable after a clone render: [" << afterRaw << "]";
            pdfe::core::MuPdfContext::releaseThreadLocalContext();
            ASSERT_GT(after, before)
                << "rendering on the thread-local CLONE did not move the glyph "
                   "cache read from the MAIN context (" << before << " -> "
                << after << "). The clone does not share the glyph cache, so "
                   "every glyph-cache bound in this file is measuring a "
                   "context nothing renders on — a decoration, not an oracle. "
                   "Fix the measurement, do not relax the assertion.";
        }

        // ── The measurements themselves.
        bool controlOk = false, fixtureOk = false;
        bool controlEvKnown = false, fixtureEvKnown = false;
        unsigned long long controlEv = 0, fixtureEv = 0;
        std::string controlRaw, fixtureRaw;
        const unsigned long long pc =
            glyphCachePopulationFor(controlStr, &controlOk, &controlRaw,
                                    &controlEvKnown, &controlEv);
        const unsigned long long px =
            glyphCachePopulationFor(fixtureStr, &fixtureOk, &fixtureRaw,
                                    &fixtureEvKnown, &fixtureEv);

        // ── VACUITY GUARD 2 — both dumps were non-empty and a number came out.
        ASSERT_TRUE(controlOk)
            << "no glyph-cache figure could be parsed for the CONTROL. The "
               "parser silently yields 0 in that case and every bound below "
               "would pass unconditionally. Raw dump: [" << controlRaw << "]";
        ASSERT_TRUE(fixtureOk)
            << "no glyph-cache figure could be parsed for " << rel
            << ". Raw dump: [" << fixtureRaw << "]";

        // ── VACUITY GUARD 3 — the control must genuinely populate the cache.
        //
        // Measured 2026-09-07: test_3page.pdf contains text and populates
        // 22,666 bytes. If it ever stops doing so, the ratio below is not
        // measuring glyph-cache population at all and must fail LOUDLY rather
        // than pass on a zero-vs-zero comparison. NOTE the ordering that makes
        // this work: MuPDF's pdf_drop_document_imp purges the whole glyph
        // cache, so the reading MUST be taken before the document is closed
        // (see dumpAfterRasterizingPageZero).
        ASSERT_GT(pc, 0ULL)
            << "the control document populated ZERO glyph-cache bytes, so the "
               "ratio below would be vacuous. Either the control fixture no "
               "longer contains text, the render did not happen, or the "
               "reading is being taken after the document (and therefore the "
               "cache) was dropped. Pick a control that populates the cache "
               "and say so here — do not weaken the assertion.";

        // ── VACUITY GUARD 4 — neither measurement SATURATED against MuPDF's
        // internal glyph-cache ceiling.
        //
        // `Glyph Cache Size` is the LIVE byte count. Once MuPDF starts evicting
        // it stops being "how much this document tried to cache" and becomes
        // "how much fits", which is a property of MuPDF, not of the fixture.
        // The ratio below would then be measuring the ceiling on both sides and
        // could not redden however pathological the fixture became. Rather than
        // estimate that ceiling from a constant, observe the eviction count the
        // dump already reports: zero evictions is direct evidence that neither
        // reading was clamped.
        //
        // If a non-zero count ever appears, the fix is to make the measurement
        // smaller (fewer glyphs, a narrower page, a purge between phases) or to
        // measure differently — NOT to raise kMaxGlyphCacheRatioVsControl,
        // which would only move the budget further into the region where it
        // cannot fail.
        //
        // The line is CONDITIONAL: libmupdf emits it only when built without
        // NDEBUG. When it is absent this guard is SKIPPED and says so, because
        // "absent" and "zero" are different facts and conflating them is how a
        // guard quietly becomes decoration.
        if (controlEvKnown && fixtureEvKnown) {
            ASSERT_EQ(controlEv, 0ULL)
                << "the CONTROL measurement evicted " << controlEv
                << " glyph(s), so its " << pc << "-byte reading is clamped by "
                   "MuPDF's internal cache ceiling rather than reflecting what "
                   "the control tried to cache. The ratio below is then not "
                   "measuring glyph-cache population at all. Lower the work or "
                   "measure differently — do NOT raise the budget, which would "
                   "only guarantee the bound can never redden. Raw dump: ["
                << controlRaw << "]";
            ASSERT_EQ(fixtureEv, 0ULL)
                << "the measurement for " << rel << " evicted " << fixtureEv
                << " glyph(s), so its " << px << "-byte reading saturated "
                   "against MuPDF's internal cache ceiling and the ratio below "
                   "is not measuring what it claims. Lower the work or measure "
                   "differently — do NOT raise the budget. Raw dump: ["
                << fixtureRaw << "]";
        } else {
            // Not a pass. An explicit, visible abstention.
            std::cout << "[  SKIPPED ] glyph-cache saturation guard for " << rel
                      << ": this libmupdf does not report \"Glyph Cache "
                         "Evictions\" (built with NDEBUG), so whether the "
                         "readings saturated is UNKNOWN — not known to be "
                         "fine. Control dump: [" << controlRaw
                      << "] fixture dump: [" << fixtureRaw << "]" << std::endl;
        }

        const double budget = static_cast<double>(pc) *
                                  kMaxGlyphCacheRatioVsControl +
                              static_cast<double>(kGlyphCacheFloorBytes);
        EXPECT_LE(static_cast<double>(px), budget)
            << "rendering " << rel << " populated " << px << " bytes of glyph "
            << "cache against a " << pc << "-byte well-formed control in the "
            << "same process (ratio "
            << (static_cast<double>(px) / static_cast<double>(pc))
            << "x, budget " << budget << " bytes). A recursive Type 3 glyph "
               "that re-enters its own content stream is expected to cache "
               "MORE than a plain text page, but not by this margin — this is "
               "the shape of recursion that is technically bounded yet still "
               "does unbounded-looking work.";
    }
};

// TIER-MARKER: 1
// (Every TEST_F below this marker, until the next TIER-MARKER, belongs to
// Tier 1. FlawCorpusCountDocs in tests/test_doc_drift_guards.cpp counts them
// per tier and asserts the counts recorded in
// tests/fixtures/flaw_corpus/COVERAGE.md and README.md still match. The doc
// counts drifted twice before that check existed — meta-audit PDF-106 /
// SEC-107 / SEC-108 cluster, phase F9.)

// 8.a — Type 3 font whose glyph content stream recursively references itself.
// The risk is unbounded recursion (DoS). MuPDF detects the cycle and returns;
// the assertion is simply that open + render COMPLETE (bounded) without throwing
// a C++ exception or hanging.
TEST_F(ExternalFlawCorpusTest, Type3RecursiveContentStream_RendersBounded) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("overlap/type3_content_stream_cycle.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    // If this returns, the recursive-type3 guard held (no infinite loop).
    EXPECT_NO_THROW({ renderPageZero(doc.get()); })
        << "recursive Type 3 font must be bounded, not crash";
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });

    // SEC-39 / GitHub #53 -- an actual wall-clock bound.
    //
    // The comment above this test claims the assertion covers "without ...
    // hanging". It did not. GoogleTest has no per-test timeout, so a document
    // that made MuPDF loop forever would have hung the whole suite rather than
    // failing this test -- and a hang in a 6,000-test run reads as a wedged
    // machine, not as this fixture's defect.
    //
    // Bounded against a CONTROL document rather than an absolute threshold,
    // reusing the helper this file already uses for the Corrupt_* family: a
    // fixed millisecond limit on a shared, loaded machine reddens at random,
    // which trains everyone to ignore the colour. Pathological recursion is
    // orders of magnitude off, not 20% off.
    expectBoundedAgainstControl("overlap/type3_content_stream_cycle.pdf");

    // Phase-12 AC4, second half: the SAME boundedness claim measured on glyph-
    // cache population rather than wall clock. Wall clock alone cannot see a
    // recursion that caches an unreasonable amount of glyph pixmaps quickly.
    expectGlyphCacheBoundedAgainstControl("overlap/type3_content_stream_cycle.pdf");
}

// 1.g — Font with a zero descent metric. Exercises baseline/line-height math
// against a degenerate metric (relevant to PDFE's text-edit overlay geometry).
TEST_F(ExternalFlawCorpusTest, ZeroDescentFontMetrics_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/zero_descent.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

// 1.g — Unusual ascent/descent metric combination (second degenerate-metrics
// case, distinct font program).
TEST_F(ExternalFlawCorpusTest, OddAscentDescentFontMetrics_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/font_ascent_descent.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

// 7.g — Clipping path (W/W*) application. Pure-vector page; the render path
// must apply the clip without crashing.
TEST_F(ExternalFlawCorpusTest, ClippingPath_RendersNoThrow) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("vector/clippath.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// 7.h — Text used AS a clipping path (text render mode 7) combined with a
// CFF/CID (Type 0) font — a compound font + clip case.
TEST_F(ExternalFlawCorpusTest, TextAsClipPathWithCidFont_RendersNoThrow) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("vector/text_clip_cff_cid.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

// 7.i — Indexed colour space referencing negative and too-high palette indices.
// The colour path must clamp/bounds-check rather than read out of range.
TEST_F(ExternalFlawCorpusTest, IndexedColorspaceOutOfRange_RendersNoThrow) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("vector/indexedcs_negative_and_high.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); })
        << "out-of-range indexed-colour indices must be bounds-checked";
}

// 2.c — Malformed /PageLabels structure. The document must still open and
// report a sane page count (PDFE's page-label editor previously assumed
// well-formed labels; a malformed one must not break the open path).
TEST_F(ExternalFlawCorpusTest, MalformedPageLabels_OpensGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("corrupt/bad_pagelabels.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

// 5.f — Certificate (public-key) encryption. PDFE/MuPDF cannot open these
// without the recipient's private key, and PDFE does not support the
// public-key handler at all. The REQUIRED behaviour is a GRACEFUL failure — a
// clean exception or a null document — NOT a crash. This test documents and
// guards that graceful-rejection contract.
TEST_F(ExternalFlawCorpusTest, CertificateEncryption_FailsGracefullyNotCrash) {
    auto path = ext_fixture("encryption/aes256_certificate.pdf");
    if (!std::filesystem::exists(path)) {
        ADD_FAILURE() << "external fixture missing: " << path.string();
        return;
    }
    std::unique_ptr<pdfe::core::Document> doc;
    bool threw = false;
    try {
        doc = pdfe::core::Document::open(ctx_, path);
    } catch (const std::exception&) {
        threw = true;  // a clean, typed failure is acceptable
    }
    // Graceful == (threw a typed exception) OR (returned null). The ONE
    // unacceptable outcome — a crash — cannot reach this assertion at all.
    EXPECT_TRUE(threw || doc == nullptr)
        << "certificate-encrypted PDF must fail gracefully (clean throw or "
           "null), not return a usable document PDFE cannot actually decrypt";
}

// ===========================================================================
// TIER-MARKER: 2
// Tier 2 — additional flaw classes (vector/shading, image rotation, annotation
// geometry, degenerate dimensions, legacy encryption revision).
// ===========================================================================

// 7.b — Mesh shading with empty/degenerate data. Gradient-mesh rendering must
// handle the empty mesh without crashing.
TEST_F(ExternalFlawCorpusTest, MeshShadingEmpty_RendersNoThrow) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("vector/mesh_shading_empty.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// 7.a — Tiling pattern with large step intervals on a wide (4000pt) page.
// The pattern tiler must not blow up on the large XStep/YStep.
TEST_F(ExternalFlawCorpusTest, TilingPatternLargeSteps_RendersBounded) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("vector/tiling_pattern_large_steps.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// 7.a-bis — GitHub #53: a tiling pattern whose content stream paints ITSELF.
//
// THE FLAW CLASS. tiling_pattern_large_steps.pdf (above) stresses the tiler's
// ARITHMETIC -- an enormous XStep/YStep on a wide page. It does not touch the
// other hostile tiling shape #53 names: unbounded RECURSION. In
// tiling_pattern_self_referential.pdf (generated by
// tests/fixtures/gen_tiling_pattern_self_referential.py) the pattern object's
// own /Resources /Pattern /P1 is that same object, and its content stream fills
// with /P1. Every nesting level is a structurally valid tile, so there is no
// syntactic error for a parser to reject; only the painter can stop it.
//
// WHY THIS TEST RASTERIZES. The sibling tests call renderPageZero(), which
// builds a display list. That is the right bound-check for the type-3 recursion
// (refused while the list is built) but it CANNOT see this one: the display
// list merely records "fill with pattern P1", and the painter descends only at
// rasterization. A display-list-only assertion here would pass on a build that
// stack-overflows on the first real paint.
//
// THE ORACLE, in three parts -- a bound alone would prove nothing here, because
// a pattern that does not recurse is trivially bounded:
//
//   NON-VACUITY  patternSelfReferences() re-derives the self-reference from the
//                object graph. If the fixture ever stops being recursive, this
//                fails instead of the bound silently passing.
//   CONTROL      the shallow sibling fixture is rasterized through the SAME
//                helper and must produce a real, non-empty raster. Without it,
//                a build that refused EVERY document would satisfy "bounded".
//   THE BOUND    the hostile render must COME BACK -- rendered or cleanly
//                refused -- inside kRasterBoundMs.
//
// MEASURED, and the two results differ in an instructive way:
//
//   MuPDF 1.26.10, the copy PDFE links (Debug, this test): the page RENDERS, in
//   6 ms. MuPDF clamps the recursion internally -- it logs "limit error:
//   exception stack overflow!" from inside the pattern painter and carries on,
//   returning a valid pixmap. So the observed outcome here is
//   rendered=1, boundedError=0.
//
//   MuPDF 1.27.2 (via PyMuPDF, out-of-process): the whole render is refused in
//   22 ms with FZ_ERROR_LIMIT propagated to the caller, i.e.
//   rendered=0, boundedError=1.
//
// Both are correct and both are BOUNDED, which is why the assertion below is
// the disjunction rather than "it must throw": pinning either one would make
// this test a version detector that reddens on an upgrade that changed nothing
// about safety. What must never happen is neither -- no return at all.
//
// The control rasterizes 4000x400 = 1,600,000 pixels in 20 ms. The 30 s bound
// is ~5000x the observed Debug-side cost, so it cannot flake, while an
// implementation that actually recursed without limit would never reach it.
//
// The one failure mode this test cannot report as a failure is a true HANG --
// it would hang with it. That is inherent to any in-process bound-check and is
// why the figure above was taken out-of-process first.
TEST_F(ExternalFlawCorpusTest, TilingPatternSelfReferential_RendersBounded) {
    constexpr long long kRasterBoundMs = 30000;

    // NON-VACUITY. Assert the fixture is genuinely self-referential BEFORE
    // drawing any conclusion from the fact that rendering it terminated.
    ASSERT_TRUE(patternSelfReferences("vector/tiling_pattern_self_referential.pdf"))
        << "the fixture's page /Pattern /P1 does not resolve to an object whose "
           "own /Resources /Pattern /P1 is itself -- it is not recursive, so the "
           "bound below would be measuring an ordinary tiling pattern";

    // CONTROL. The shallow sibling must genuinely rasterize through the same
    // helper. If this fails, the helper or the build is refusing everything and
    // nothing below is evidence of anything.
    const auto control = rasterizePageZero("vector/tiling_pattern_large_steps.pdf");
    ASSERT_TRUE(control.opened) << "control fixture would not open";
    ASSERT_TRUE(control.rendered)
        << "the control produced NO raster, so 'the hostile one was bounded' "
           "would be indistinguishable from 'this build renders nothing'";
    ASSERT_GT(control.pixels, 100000)
        << "the control raster is too small to be a real render of a 4000x400 "
           "page; got " << control.pixels << " pixels";
    ASSERT_LT(control.elapsedMs, kRasterBoundMs)
        << "the control itself exceeded the bound, so the bound is not "
           "measuring the recursion";

    // THE BOUND.
    const auto hostile = rasterizePageZero("vector/tiling_pattern_self_referential.pdf");
    EXPECT_TRUE(hostile.opened)
        << "the self-referential fixture would not even open -- the recursion is "
           "in the PAINTER, so it must be reachable past the parser for this "
           "test to exercise anything";
    EXPECT_TRUE(hostile.rendered || hostile.boundedError)
        << "rasterizing the self-referential pattern neither produced a raster "
           "nor reported a clean refusal";
    EXPECT_LT(hostile.elapsedMs, kRasterBoundMs)
        << "rasterizing a self-referential tiling pattern took "
        << hostile.elapsedMs << " ms, over the " << kRasterBoundMs
        << " ms bound -- the pattern painter is descending without a limit";

    // Publish the outcome so a later run can be compared with this one rather
    // than merely re-asserting the same pass/fail.
    RecordProperty("selfref_rendered", hostile.rendered ? 1 : 0);
    RecordProperty("selfref_bounded_error", hostile.boundedError ? 1 : 0);
    RecordProperty("selfref_elapsed_ms", std::to_string(hostile.elapsedMs));
    RecordProperty("control_elapsed_ms", std::to_string(control.elapsedMs));
    RecordProperty("control_pixels", std::to_string(control.pixels));
}

// 7.d — Axial/radial shading with the /Extend flag painting beyond the defined
// gradient bounds.
TEST_F(ExternalFlawCorpusTest, ShadingExtend_RendersNoThrow) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("vector/shading_extend.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// 3.f — Rotated bitonal (black/white) image. Exercises image rotation + color
// handling together (PDFE offers image rotation as an edit op).
TEST_F(ExternalFlawCorpusTest, RotatedBitonalImage_OpenRender) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/rotated_bw_ratio.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// 8.d — Rotated free-text annotations, which commonly overlap page content when
// rotation is not applied consistently to the bounding box.
TEST_F(ExternalFlawCorpusTest, RotatedFreeTextAnnotations_RendersNoThrow) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("overlap/rotated_freetexts.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// 8.e — Zero-width line stroke (degenerate geometry). A width-0 stroke must not
// crash the rasterizer.
TEST_F(ExternalFlawCorpusTest, ZeroWidthLineStroke_RendersNoThrow) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("overlap/zerowidthline.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// 10.d — Checkbox form field with a malformed appearance stream. Opening +
// rendering must not crash even though the widget's /AP is bad (the renderer
// may need to synthesize/skip the appearance).
TEST_F(ExternalFlawCorpusTest, CheckboxMalformedAppearance_OpenRenders) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("forms/checkbox_bad_appearance.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// 1.f — TrueType font with complex glyph tables / hinting.
TEST_F(ExternalFlawCorpusTest, ComplexTrueTypeHinting_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/complex_ttf.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

// 2.d — Header says %PDF-1.8 but the file is really 1.7. Version sniffing must
// not break the open path (the document is otherwise valid).
TEST_F(ExternalFlawCorpusTest, HeaderVersionMismatch_OpensGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("corrupt/header_version_mismatch.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_GE(doc->pageCount(), 1);
}

// 6.a — Real MediaBox exceeding the 14400-unit limit (14401 wide). The "very
// large page" case; render must stay bounded, not allocate unboundedly.
TEST_F(ExternalFlawCorpusTest, HugeMediaBoxOver14400_RendersBounded) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("dimensions/mediabox_over_14400.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// 6.b — ArtBox dimensions below 3 units (degenerate tiny region). Must open
// without crashing.
TEST_F(ExternalFlawCorpusTest, TinyArtBoxUnder3_OpensGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("dimensions/artbox_under_3.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// 6.d — A real number below the PDF/A minimum in document structure (numeric
// underflow). Must open without crashing.
TEST_F(ExternalFlawCorpusTest, NumericUnderflowInStructure_OpensGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("dimensions/real_underflow_structure.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// 5.d — AES-256 REVISION 5 (the deprecated Adobe extension, /V 5 /R 5), which
// is a DISTINCT decryption path from PDFE's R6 fixture. PDFE must decrypt it
// with the correct password, and reject it gracefully without one.
TEST_F(ExternalFlawCorpusTest, Aes256Revision5_DecryptsWithPassword) {
    auto path = ext_fixture("encryption/aes256_r5.pdf");
    if (!std::filesystem::exists(path)) {
        ADD_FAILURE() << "external fixture missing: " << path.string();
        return;
    }
    // Without the password: graceful failure (clean throw or null), no crash.
    std::unique_ptr<pdfe::core::Document> noPw;
    bool threw = false;
    try {
        noPw = pdfe::core::Document::open(ctx_, path);
    } catch (const std::exception&) {
        threw = true;
    }
    EXPECT_TRUE(threw || noPw == nullptr)
        << "R5-encrypted file must not open as usable without a password";

    // With the correct user password ("user"): opens and reads the page.
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = pdfe::core::Document::open(ctx_, path, "user"); })
        << "AES-256 R5 decryption with the correct password must succeed";
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
}

// ===========================================================================
// TIER-MARKER: 3
// Tier 3 — Fonts (§1), Encryption/permissions (§5), Structural corruption (§2).
// Fixtures downloaded from the exact upstream URLs in pdf-test-corpus-reference.md.
// ===========================================================================

// ---- §1 Fonts ----------------------------------------------------------------
// Open + render + extract must succeed regardless of embedding state / font
// class. Text is present in all of these, so extraction must return without
// throwing (content may or may not be recoverable when a font is not embedded,
// but the pipeline must not crash).

TEST_F(ExternalFlawCorpusTest, Font_NotEmbeddedSingle_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/not_embedded_single.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

TEST_F(ExternalFlawCorpusTest, Font_EmbeddedFull_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/embedded_full.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

TEST_F(ExternalFlawCorpusTest, Font_EmbeddedSubset_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/embedded_subset.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

TEST_F(ExternalFlawCorpusTest, Font_ArialTimesNotEmbedded_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/arial_times_not_embedded.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 56);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

TEST_F(ExternalFlawCorpusTest, Font_CalistoNotEmbedded_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/calisto_not_embedded.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

TEST_F(ExternalFlawCorpusTest, Font_PdfA1aEmbedded_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/pdfa1a_embedded.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

TEST_F(ExternalFlawCorpusTest, Font_MixedFonts_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/mixed_fonts.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

// Type 3 (procedural/bitmap) font — a class many renderers mishandle.
TEST_F(ExternalFlawCorpusTest, Font_SimpleType3_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/simple_type3.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

// CID font whose CMap overflows expected ranges — bounds handling in CJK/CID
// text extraction.
TEST_F(ExternalFlawCorpusTest, Font_CidCmapOverflow_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/cidfont_cmap_overflow.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

// Composite (Type 0) CID font, Arabic-range mapping.
TEST_F(ExternalFlawCorpusTest, Font_ArialUnicodeCidArabic_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/arial_unicode_cid_ab.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

// Composite CID font, Latin-range mapping (pairs with the Arabic case).
TEST_F(ExternalFlawCorpusTest, Font_ArialUnicodeCidLatin_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/arial_unicode_cid_en.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

// The 14 standard PDF fonts as a baseline/regression reference.
TEST_F(ExternalFlawCorpusTest, Font_StandardFonts_OpenRenderExtract) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("fonts/standard_fonts.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 14);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

// ---- §5 Encryption / access-control -----------------------------------------

// open-password: requires a password just to OPEN. Upstream does not publish
// the password, so we assert the security-critical behavior: PDFE must NOT
// silently yield a usable document without one — it throws (or returns null),
// never crashes.
TEST_F(ExternalFlawCorpusTest, Enc_OpenPassword_RejectedWithoutPassword) {
    auto path = ext_fixture("encryption/open_password.pdf");
    if (!std::filesystem::exists(path)) {
        ADD_FAILURE() << "external fixture missing: " << path.string();
        return;
    }
    std::unique_ptr<pdfe::core::Document> doc;
    bool threw = false;
    try {
        doc = pdfe::core::Document::open(ctx_, path);
    } catch (const std::exception&) {
        threw = true;
    }
    EXPECT_TRUE(threw || doc == nullptr)
        << "a file that requires an open-password must not open as usable "
           "without one";
}

// RC4 40-bit standard security handler (legacy/weak-algorithm compatibility).
TEST_F(ExternalFlawCorpusTest, Enc_Standard40_DecryptsWithUserPassword) {
    auto path = ext_fixture("encryption/standard40.pdf");
    if (!std::filesystem::exists(path)) {
        ADD_FAILURE() << "external fixture missing: " << path.string();
        return;
    }
    // Wrong/no password must fail gracefully, not crash.
    std::unique_ptr<pdfe::core::Document> noPw;
    bool threw = false;
    try { noPw = pdfe::core::Document::open(ctx_, path); }
    catch (const std::exception&) { threw = true; }
    EXPECT_TRUE(threw || noPw == nullptr);
    // Correct user password opens it.
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = pdfe::core::Document::open(ctx_, path, "user"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
    pdfe::encryption::EncryptionManager mgr(ctx_);
    EXPECT_TRUE(mgr.isEncrypted(doc.get()));
}

// RC4 128-bit standard security handler.
TEST_F(ExternalFlawCorpusTest, Enc_Standard128_DecryptsWithUserPassword) {
    auto path = ext_fixture("encryption/standard128.pdf");
    if (!std::filesystem::exists(path)) {
        ADD_FAILURE() << "external fixture missing: " << path.string();
        return;
    }
    std::unique_ptr<pdfe::core::Document> noPw;
    bool threw = false;
    try { noPw = pdfe::core::Document::open(ctx_, path); }
    catch (const std::exception&) { threw = true; }
    EXPECT_TRUE(threw || noPw == nullptr);
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = pdfe::core::Document::open(ctx_, path, "user"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
    pdfe::encryption::EncryptionManager mgr(ctx_);
    EXPECT_TRUE(mgr.isEncrypted(doc.get()));
}

// AES-128, password-based (distinct algorithm path from RC4 and AES-256).
TEST_F(ExternalFlawCorpusTest, Enc_Aes128_DecryptsWithUserPassword) {
    auto path = ext_fixture("encryption/aes128.pdf");
    if (!std::filesystem::exists(path)) {
        ADD_FAILURE() << "external fixture missing: " << path.string();
        return;
    }
    std::unique_ptr<pdfe::core::Document> noPw;
    bool threw = false;
    try { noPw = pdfe::core::Document::open(ctx_, path); }
    catch (const std::exception&) { threw = true; }
    EXPECT_TRUE(threw || noPw == nullptr);
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = pdfe::core::Document::open(ctx_, path, "user"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
    pdfe::encryption::EncryptionManager mgr(ctx_);
    EXPECT_TRUE(mgr.isEncrypted(doc.get()));
}

// no-copy: opens WITHOUT a password but the copy/extract permission bit is
// cleared. PDFE must surface that via getPermissions().copy == false while
// leaving unrelated permissions (print) intact.
TEST_F(ExternalFlawCorpusTest, Enc_NoCopy_CopyPermissionDenied) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("encryption/no_copy.pdf"); });
    ASSERT_NE(doc, nullptr);
    pdfe::encryption::EncryptionManager mgr(ctx_);
    ASSERT_TRUE(mgr.isEncrypted(doc.get()));
    auto perms = mgr.getPermissions(doc.get());
    EXPECT_FALSE(perms.copy) << "no-copy fixture must report copy permission denied";
    EXPECT_TRUE(perms.print) << "unrelated print permission must remain allowed";
}

// no-printing: print (and high-quality print) permission cleared.
TEST_F(ExternalFlawCorpusTest, Enc_NoPrinting_PrintPermissionDenied) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("encryption/no_printing.pdf"); });
    ASSERT_NE(doc, nullptr);
    pdfe::encryption::EncryptionManager mgr(ctx_);
    ASSERT_TRUE(mgr.isEncrypted(doc.get()));
    auto perms = mgr.getPermissions(doc.get());
    EXPECT_FALSE(perms.print) << "no-printing fixture must report print permission denied";
    EXPECT_FALSE(perms.printHighQuality);
    EXPECT_TRUE(perms.copy) << "unrelated copy permission must remain allowed";
}

// no-text-access: accessibility (screen-reader text extraction) permission
// cleared. This is the permission a text-extraction pipeline must respect.
TEST_F(ExternalFlawCorpusTest, Enc_NoTextAccess_AccessibilityPermissionDenied) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("encryption/no_text_access.pdf"); });
    ASSERT_NE(doc, nullptr);
    pdfe::encryption::EncryptionManager mgr(ctx_);
    ASSERT_TRUE(mgr.isEncrypted(doc.get()));
    auto perms = mgr.getPermissions(doc.get());
    EXPECT_FALSE(perms.accessibility)
        << "no-text-access fixture must report accessibility permission denied";
    EXPECT_TRUE(perms.print) << "unrelated print permission must remain allowed";
}

// ---- §2 Structural corruption / broken indexing -----------------------------
// MuPDF's repair path recovers all of these; the contract is graceful
// RECOVERY (open succeeds, page tree readable, render does not crash) rather
// than a hard failure or crash.

TEST_F(ExternalFlawCorpusTest, Corrupt_OneByteMissing_RecoversGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("corrupt/one_byte_missing.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    doc.reset();  // release the handle before the timed re-parses
    // PDF-87: bounded work, measured against a same-run control.
    expectBoundedAgainstControl("corrupt/one_byte_missing.pdf");

    // #126: this fixture IS part of the recovery class -- it forces MuPDF to
    // reconstruct the xref. Asserted, not described, so the claim reddens if
    // it stops being true.
    EXPECT_TRUE(repairPathTaken("corrupt/one_byte_missing.pdf"))
        << "COVERAGE.md counts this fixture as recovery coverage, but MuPDF "
           "opened it WITHOUT rebuilding the xref. Either the fixture was "
           "regenerated into something MuPDF can parse directly, or the "
           "repair path changed. Recovery coverage is now smaller than the "
           "docs claim.";
}

// The `xref` keyword itself is missing — forces a full-file object rebuild.
TEST_F(ExternalFlawCorpusTest, Corrupt_XrefCommandMissing_RebuildsAndOpens) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("corrupt/xref_command_missing.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    doc.reset();  // release the handle before the timed re-parses
    // PDF-87: bounded work, measured against a same-run control.
    expectBoundedAgainstControl("corrupt/xref_command_missing.pdf");

    // #126: this fixture IS part of the recovery class -- it forces MuPDF to
    // reconstruct the xref. Asserted, not described, so the claim reddens if
    // it stops being true.
    EXPECT_TRUE(repairPathTaken("corrupt/xref_command_missing.pdf"))
        << "COVERAGE.md counts this fixture as recovery coverage, but MuPDF "
           "opened it WITHOUT rebuilding the xref. Either the fixture was "
           "regenerated into something MuPDF can parse directly, or the "
           "repair path changed. Recovery coverage is now smaller than the "
           "docs claim.";
}

// Stream declares a filter chain but zero-length data — filter-pipeline edge.
TEST_F(ExternalFlawCorpusTest, Corrupt_MultipleFiltersLengthZero_OpensRenders) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("corrupt/multiple_filters_length_zero.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    doc.reset();  // release the handle before the timed re-parses
    // PDF-87: bounded work, measured against a same-run control.
    expectBoundedAgainstControl("corrupt/multiple_filters_length_zero.pdf");

    // #126: this fixture is malformed in a way MuPDF parses WITHOUT rebuilding
    // the xref, so it is NOT part of the recovery class. Pinned in that
    // direction too: if it starts repairing, the recovery count in COVERAGE.md
    // is stale in the other direction and the table needs updating.
    EXPECT_FALSE(repairPathTaken("corrupt/multiple_filters_length_zero.pdf"))
        << "this fixture now forces an xref rebuild, which it did not before. "
           "That is not a failure in itself, but COVERAGE.md records it as "
           "NOT exercising recovery, and that is now wrong.";
}

TEST_F(ExternalFlawCorpusTest, Corrupt_HelloWorldBad_RecoversGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("corrupt/helloworld_bad.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    doc.reset();  // release the handle before the timed re-parses
    // PDF-87: bounded work, measured against a same-run control.
    expectBoundedAgainstControl("corrupt/helloworld_bad.pdf");

    // #126: this fixture IS part of the recovery class -- it forces MuPDF to
    // reconstruct the xref. Asserted, not described, so the claim reddens if
    // it stops being true.
    EXPECT_TRUE(repairPathTaken("corrupt/helloworld_bad.pdf"))
        << "COVERAGE.md counts this fixture as recovery coverage, but MuPDF "
           "opened it WITHOUT rebuilding the xref. Either the fixture was "
           "regenerated into something MuPDF can parse directly, or the "
           "repair path changed. Recovery coverage is now smaller than the "
           "docs claim.";
}

TEST_F(ExternalFlawCorpusTest, Corrupt_ScanBad_RecoversGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("corrupt/scan_bad.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    doc.reset();  // release the handle before the timed re-parses
    // PDF-87: bounded work, measured against a same-run control.
    expectBoundedAgainstControl("corrupt/scan_bad.pdf");

    // #126: this fixture IS part of the recovery class -- it forces MuPDF to
    // reconstruct the xref. Asserted, not described, so the claim reddens if
    // it stops being true.
    EXPECT_TRUE(repairPathTaken("corrupt/scan_bad.pdf"))
        << "COVERAGE.md counts this fixture as recovery coverage, but MuPDF "
           "opened it WITHOUT rebuilding the xref. Either the fixture was "
           "regenerated into something MuPDF can parse directly, or the "
           "repair path changed. Recovery coverage is now smaller than the "
           "docs claim.";
}

// Real-world parser-bug regression case (pdf.js #1586).
TEST_F(ExternalFlawCorpusTest, Corrupt_PdfJsBad1586_RecoversGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("corrupt/pdfjsbad1586.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    doc.reset();  // release the handle before the timed re-parses
    // PDF-87: bounded work, measured against a same-run control.
    expectBoundedAgainstControl("corrupt/pdfjsbad1586.pdf");

    // #126: this fixture is malformed in a way MuPDF parses WITHOUT rebuilding
    // the xref, so it is NOT part of the recovery class. Pinned in that
    // direction too: if it starts repairing, the recovery count in COVERAGE.md
    // is stale in the other direction and the table needs updating.
    EXPECT_FALSE(repairPathTaken("corrupt/pdfjsbad1586.pdf"))
        << "this fixture now forces an xref rebuild, which it did not before. "
           "That is not a failure in itself, but COVERAGE.md records it as "
           "NOT exercising recovery, and that is now wrong.";
}

// Invalid bounding-box values on content — geometry sanity-checking.
TEST_F(ExternalFlawCorpusTest, Corrupt_BoundingBoxInvalid_OpensRenders) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("corrupt/boundingbox_invalid.pdf"); });
    ASSERT_NE(doc, nullptr);
    EXPECT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    doc.reset();  // release the handle before the timed re-parses
    // PDF-87: bounded work, measured against a same-run control.
    expectBoundedAgainstControl("corrupt/boundingbox_invalid.pdf");

    // #126: this fixture is malformed in a way MuPDF parses WITHOUT rebuilding
    // the xref, so it is NOT part of the recovery class. Pinned in that
    // direction too: if it starts repairing, the recovery count in COVERAGE.md
    // is stale in the other direction and the table needs updating.
    EXPECT_FALSE(repairPathTaken("corrupt/boundingbox_invalid.pdf"))
        << "this fixture now forces an xref rebuild, which it did not before. "
           "That is not a failure in itself, but COVERAGE.md records it as "
           "NOT exercising recovery, and that is now wrong.";
}

// ===========================================================================
// TIER-MARKER: 4
// Tier-4 — the COVERAGE.md "Deferred" classes that fit the fetch-upstream +
// asserting-test pattern (image codecs, numeric edges, transparency/blend,
// degenerate annotations, embedded files, AcroForm/XFA, and a JS-safety
// negative test). Contract, as for Tiers 1-3: open/render/extract must not
// CRASH or HANG. A genuinely-unsupported codec may render nothing, but must
// fail GRACEFULLY (no throw, no loop). Malicious datasets and PDF bombs are
// deliberately EXCLUDED (see COVERAGE.md §12/§13) — they need a sandbox harness.
// ===========================================================================

// --- §3 Image codecs -------------------------------------------------------

// JBIG2 (scanned-document bitonal codec). Must decode-or-degrade gracefully.
TEST_F(ExternalFlawCorpusTest, Jbig2FileHeader_OpenRendersGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/jbig2-file-header.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); }) << "JBIG2 decode must not crash";
}

// JBIG2 with a symbol-dictionary offset edge case (known decoder-bug source).
TEST_F(ExternalFlawCorpusTest, Jbig2SymbolOffset_OpenRendersGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/jbig2-symbol-offset.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// JPEG2000 (JPX) decode regression case — #50.
// RE-ENABLED (was DISABLED_): the security fix is the MuPDF decode gate
// (vcpkg-overlay-ports/libmupdf, 0001-pdfe-jpx-decode-gate.patch). images/jpx-bug.pdf
// (pdf.js bug_jpx.pdf, a malformed JPEG2000) used to trip a heap-buffer-overflow INSIDE
// openjp2.dll during decode (STATUS_HEAP_CORRUPTION 0xC0000374; fz_try/fz_catch cannot
// trap heap corruption). PDFE now ships with JPX decoding DISABLED BY DEFAULT: while
// off, fz_load_jpx returns a placeholder WITHOUT entering openjpeg, so the malformed
// stream is never decoded and the process cannot be corrupted. This test asserts that
// default-safe contract: JPX is off by default, and opening + rendering the known
// crasher does NOT crash. (We deliberately do NOT flip the flag on here — enabling it
// would re-expose the openjpeg bug on this hostile fixture; the opt-in path is covered
// by the preference tests.) Rationale: docs/audit/dca/0011-jpx-openjpeg-heap-overflow.md.
TEST_F(ExternalFlawCorpusTest, JpxDecode_OpenRendersGracefully) {
    // Precondition: JPX decoding is disabled by default (the security posture).
    EXPECT_FALSE(pdfe::core::isJpxDecodingEnabled())
        << "JPX decoding must be OFF by default (#50): a malformed JPX must not reach openjpeg";

    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/jpx-bug.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    // With JPX gated off, the malformed stream is never decoded → no heap
    // corruption. Rendering must complete and produce a valid display list.
    EXPECT_NO_THROW({ renderPageZero(doc.get()); })
        << "JPX decode gate must render the malformed fixture gracefully (no openjpeg call)";
}

// #50: the JPX decode gate flag defaults OFF and the setter/getter round-trips.
// We deliberately do NOT decode any JPX while enabled here — this asserts only the
// flag mechanism the PDFE preference drives; the hostile fixture stays untouched.
TEST_F(ExternalFlawCorpusTest, JpxDecodeGate_DefaultOffAndToggles) {
    EXPECT_FALSE(pdfe::core::isJpxDecodingEnabled()) << "must default to disabled";
    pdfe::core::setJpxDecodingEnabled(true);
    EXPECT_TRUE(pdfe::core::isJpxDecodingEnabled());
    pdfe::core::setJpxDecodingEnabled(false);
    EXPECT_FALSE(pdfe::core::isJpxDecodingEnabled()) << "must return to disabled";
}

// #50 phase-4: the image enumerator flags JPXDecode images so the render layer can
// overlay a "disabled for security" placeholder over the hidden image.
TEST_F(ExternalFlawCorpusTest, JpxImage_DetectedByEnumerator) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/jpx-bug.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    pdfe::image::ImageManager mgr(doc->context());
    bool anyJpx = false;
    for (const auto& info : mgr.enumerateImages(page.get(), doc.get())) {
        if (info.isJpx) anyJpx = true;
    }
    EXPECT_TRUE(anyJpx)
        << "jpx-bug.pdf must expose a JPXDecode image to the enumerator (drives the overlay)";
}

// ── SEC-84 / GitHub #65 ─────────────────────────────────────────────────────
//
// A POSITIVE oracle for the JPX gate. Everything else about #50 is negative:
// "opens without crashing", "renders gracefully", "the overlay region is
// non-empty". All of those would ALSO pass if MuPDF had silently dropped the
// image entirely, or -- much worse -- if the gate had regressed and openjpeg
// really had decoded the malformed stream without happening to corrupt the heap
// on that run. #50's crash was probabilistic (~53% of runs), so "it did not
// crash this time" is not evidence the decode did not happen.
//
// This asserts what the gate CLAIMS to produce: the decoded pixmap is the 1x1
// placeholder that `fz_load_jpx` returns while JPX is off (JpxSecurityGate.h),
// NOT a real decode of the image's declared dimensions.
//
// The declared size is read from the XObject dictionary, which the gate does not
// touch, so declared-vs-decoded is a contrast within one run rather than a bare
// magic number: if they are equal, a real decode happened.
//
// DELIBERATELY DOES NOT ENABLE JPX. Toggling the gate on and decoding this
// fixture is the heap overflow (#50) itself; the contrast is obtained from the
// dictionary instead.
TEST_F(ExternalFlawCorpusTest, JpxDecode_YieldsThePlaceholderNotARealDecode) {
    ASSERT_FALSE(pdfe::core::isJpxDecodingEnabled())
        << "this oracle is about the DEFAULT posture; JPX must be off here";

    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/jpx-bug.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    pdfe::image::ImageManager mgr(doc->context());
    const auto images = mgr.enumerateImages(page.get(), doc.get());

    int jpxIndex = -1;
    for (const auto& info : images) {
        if (info.isJpx) { jpxIndex = info.imageIndex; break; }
    }
    ASSERT_GE(jpxIndex, 0)
        << "no JPXDecode image found, so this test would pass vacuously";

    // The declared size comes from the XObject DICTIONARY, not the enumerator:
    // enumerateImages reports the DECODED size, which for a gated JPX image is
    // already the 1x1 placeholder. Using it would compare 1 against 1.
    const auto declared = declaredImageSize("images/jpx-bug.pdf");
    const int declaredW = declared.first;
    const int declaredH = declared.second;
    ASSERT_GT(declaredW * declaredH, 1)
        << "the fixture's image XObject declares " << declaredW << "x"
        << declaredH << "; a 1x1 declaration would make the decoded-vs-declared "
           "contrast below vacuous";

    const QImage decoded = mgr.extractImage(page.get(), doc.get(), jpxIndex);

    ASSERT_FALSE(decoded.isNull())
        << "the gate must return a placeholder pixmap, not nothing: a null "
           "image would mean the image vanished rather than being replaced, "
           "which is a different (and unasserted) behaviour";

    EXPECT_EQ(decoded.width(), 1);
    EXPECT_EQ(decoded.height(), 1)
        << "with JPX disabled, fz_load_jpx must return the 1x1 placeholder. A "
           "decode at the declared " << declaredW << "x" << declaredH
        << " means the gate did NOT hold and the malformed JPEG2000 stream "
           "reached openjpeg -- the #50 heap-overflow path. Note the crash is "
           "probabilistic, so a clean run is not evidence of safety; this size "
           "check is.";

    EXPECT_FALSE(decoded.width() == declaredW && decoded.height() == declaredH)
        << "the decoded pixmap matches the DECLARED dimensions, which is what a "
           "real decode looks like";
}

// #50 phase-4: the placeholder-overlay region set is non-empty when JPX is
// disabled (default — the blank spot gets the notice) and empty when enabled
// (the real image is shown). Static, Document-based — no live widget needed.
TEST_F(ExternalFlawCorpusTest, JpxPlaceholderOverlay_GatedByPreference) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/jpx-bug.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);

    const bool saved = pdfe::core::isJpxDecodingEnabled();

    pdfe::core::setJpxDecodingEnabled(false);
    EXPECT_FALSE(pdfe::ui::PdfCanvas::jpxPlaceholderRectsForPage(doc.get(), 0).isEmpty())
        << "disabled JPX must yield a placeholder region over the hidden image";

    pdfe::core::setJpxDecodingEnabled(true);
    EXPECT_TRUE(pdfe::ui::PdfCanvas::jpxPlaceholderRectsForPage(doc.get(), 0).isEmpty())
        << "enabled JPX shows the real image — no placeholder overlay";

    pdfe::core::setJpxDecodingEnabled(saved);
}

// #50 phase-5: end-to-end at the shipping default (JPX OFF) — open + render the
// known crasher without a heap-corruption crash, document unmutated, AND the
// render layer exposes a placeholder region over the hidden JPX image. Ties the
// decode gate (phase-2) and the overlay (phase-4) together in one flow.
TEST_F(ExternalFlawCorpusTest, JpxEndToEnd_DefaultOffRendersWithPlaceholder) {
    const bool saved = pdfe::core::isJpxDecodingEnabled();
    pdfe::core::setJpxDecodingEnabled(false);  // the shipping default

    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/jpx-bug.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_FALSE(doc->isModified()) << "opening must not mutate the document";
    EXPECT_NO_THROW({ renderPageZero(doc.get()); })
        << "must render the crasher without a heap-corruption crash";
    EXPECT_FALSE(pdfe::ui::PdfCanvas::jpxPlaceholderRectsForPage(doc.get(), 0).isEmpty())
        << "the hidden JPX image must get a placeholder region at the default (off)";

    pdfe::core::setJpxDecodingEnabled(saved);
}

// ── SEC-56 / GitHub #56: the JBIG2 decode gate ──────────────────────────────
//
// Mirrors #50/JPX above: jbig2dec has a long history of memory-safety CVEs in
// its generic-region and symbol-dictionary decoders, MuPDF's fz_try/fz_catch
// cannot trap the heap corruption they produce, so PDFE gates the decoder OFF
// by default and substitutes a blank placeholder
// (vcpkg-overlay-ports/libmupdf/0002-pdfe-jbig2-decode-gate.patch).
//
// WHY THE ORACLE IS NOT "IT RENDERED". /JBIG2Decode is a STREAM FILTER, not a
// whole-image codec, so — unlike the gated JPX path — the gate cannot be seen
// in the decoded image's DIMENSIONS: the pixmap is still the declared
// /Width x /Height either way. "Opens", "renders", "has an overlay region"
// would every one of them stay green if the gate had been dropped and jbig2dec
// really had chewed through the malformed stream without corrupting the heap on
// that particular run. So the oracle here is twofold and positive:
//
//   1. fz_pdfe_jbig2_placeholder_count() — incremented by the patch exactly
//      once per substitution. A non-increment means the real decoder ran.
//   2. the returned pixels are INK-FREE (the placeholder emits all-1 bits =
//      white), where a real decode of these fixtures produces black.
//
// Test 4 below pins the contrast directly, on a trusted authored fixture.

namespace {
// Darkest sample in a grayscale/colour QImage, 0 = black, 255 = white.
// Returns 256 for a null image so a caller cannot mistake "nothing" for "white".
int darkestSample(const QImage& img) {
    if (img.isNull()) return 256;
    const QImage gray = img.convertToFormat(QImage::Format_Grayscale8);
    int darkest = 255;
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* row = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            if (row[x] < darkest) darkest = row[x];
        }
    }
    return darkest;
}
}  // namespace

// The gate flag defaults OFF and the setter/getter round-trips. Deliberately
// decodes nothing while enabled — this asserts the flag contract only.
TEST_F(ExternalFlawCorpusTest, Jbig2DecodeGate_DefaultOffAndToggles) {
    EXPECT_FALSE(pdfe::core::isJbig2DecodingEnabled()) << "must default to disabled";
    pdfe::core::setJbig2DecodingEnabled(true);
    EXPECT_TRUE(pdfe::core::isJbig2DecodingEnabled());
    pdfe::core::setJbig2DecodingEnabled(false);
    EXPECT_FALSE(pdfe::core::isJbig2DecodingEnabled()) << "must return to disabled";
}

// The image enumerator flags JBIG2Decode images so the render layer can overlay
// them (the same seam the JPX overlay uses).
TEST_F(ExternalFlawCorpusTest, Jbig2Image_DetectedByEnumerator) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/jbig2-file-header.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    pdfe::image::ImageManager mgr(doc->context());
    bool anyJbig2 = false;
    for (const auto& info : mgr.enumerateImages(page.get(), doc.get())) {
        if (info.isJbig2) anyJbig2 = true;
    }
    EXPECT_TRUE(anyJbig2)
        << "jbig2-file-header.pdf must expose a JBIG2Decode image to the "
           "enumerator (it drives the placeholder overlay)";
}

// THE LOAD-BEARING ORACLE. Asserts what the gate CLAIMS to produce on the
// hostile corpus fixture: the placeholder substitution HAPPENED (counter) and
// the pixels are ink-free — not merely that the process survived.
//
// DELIBERATELY DOES NOT ENABLE JBIG2. Turning the gate on and feeding this
// fixture to jbig2dec is the very thing the gate exists to prevent; the
// positive contrast comes from a trusted authored fixture in the next test.
TEST_F(ExternalFlawCorpusTest, Jbig2Decode_YieldsThePlaceholderNotARealDecode) {
    ASSERT_FALSE(pdfe::core::isJbig2DecodingEnabled())
        << "this oracle is about the DEFAULT posture; JBIG2 must be off here";

    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/jbig2-file-header.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    pdfe::image::ImageManager mgr(doc->context());
    int jbig2Index = -1;
    for (const auto& info : mgr.enumerateImages(page.get(), doc.get())) {
        if (info.isJbig2) { jbig2Index = info.imageIndex; break; }
    }
    ASSERT_GE(jbig2Index, 0)
        << "no JBIG2Decode image found, so this test would pass vacuously";

    const int before = pdfe::core::jbig2PlaceholderCount();
    const QImage decoded = mgr.extractImage(page.get(), doc.get(), jbig2Index);
    const int after = pdfe::core::jbig2PlaceholderCount();

    ASSERT_FALSE(decoded.isNull())
        << "the gate must return a placeholder image, not nothing: a null image "
           "would mean the image vanished rather than being replaced, which is "
           "a different (and unasserted) behaviour";

    EXPECT_GT(after, before)
        << "MuPDF did NOT substitute the JBIG2 placeholder while the gate was "
           "off, which means the /JBIG2Decode stream was handed to jbig2dec — "
           "the SEC-56 path. The corruption jbig2dec is gated against is "
           "input-dependent, so a clean run is not evidence of safety; this "
           "counter is. Check that 0002-pdfe-jbig2-decode-gate.patch is still "
           "applied by vcpkg-overlay-ports/libmupdf/portfile.cmake.";

    EXPECT_EQ(darkestSample(decoded), 255)
        << "the gated JBIG2 image decoded to something with ink in it "
           "(darkest sample " << darkestSample(decoded)
        << "). The placeholder emits all-1 bits, i.e. a blank image; any black "
           "pixel means real coded data was decoded.";
}

// THE CONTRAST, on tests/fixtures/jbig2_redaction.pdf — an AUTHORED, trusted
// 64x64 bilevel image whose top-left quadrant is solid black
// (tests/fixtures/gen_jbig2_redaction.py). Enabling the gate on THIS input is
// safe, and it is what makes the ink-free assertion above meaningful: without
// it, "no black pixels" could just as well describe a fixture that has no black
// pixels to begin with, and the counter could be incrementing on a path that
// nothing renders.
//
// Each posture gets a FRESH context + Document so MuPDF's per-context image
// store cannot serve the first posture's pixmap to the second.
TEST_F(ExternalFlawCorpusTest, Jbig2Decode_RealDecodeIsDistinguishableFromThePlaceholder) {
    const auto fixture = std::filesystem::path(TEST_FIXTURES_DIR) / "jbig2_redaction.pdf";
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << "fixture missing (regenerate with: python "
           "tests/fixtures/gen_jbig2_redaction.py): " << fixture.string();

    // Returns {darkest sample, placeholder-count delta} for a fresh decode.
    auto decodeOnce = [&fixture](bool gateEnabled) -> std::pair<int, int> {
        pdfe::core::setJbig2DecodingEnabled(gateEnabled);
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, fixture);
        if (!doc || doc->pageCount() < 1) return {-1, -1};
        auto page = doc->page(0);
        if (!page) return {-1, -1};
        pdfe::image::ImageManager mgr(doc->context());
        int idx = -1;
        for (const auto& info : mgr.enumerateImages(page.get(), doc.get())) {
            if (info.isJbig2) { idx = info.imageIndex; break; }
        }
        if (idx < 0) return {-1, -1};
        const int before = pdfe::core::jbig2PlaceholderCount();
        const QImage img = mgr.extractImage(page.get(), doc.get(), idx);
        const int delta = pdfe::core::jbig2PlaceholderCount() - before;
        return {darkestSample(img), delta};
    };

    const bool saved = pdfe::core::isJbig2DecodingEnabled();

    const auto gated = decodeOnce(false);
    const auto real = decodeOnce(true);

    pdfe::core::setJbig2DecodingEnabled(saved);

    ASSERT_GE(gated.first, 0) << "gated decode did not reach the JBIG2 image";
    ASSERT_GE(real.first, 0) << "real decode did not reach the JBIG2 image";

    EXPECT_LT(real.first, 128)
        << "with JBIG2 ENABLED the authored fixture must decode to its real "
           "black-quadrant content (darkest sample " << real.first
        << "). If this is white, the contrast below is vacuous and the "
           "placeholder assertions prove nothing.";
    EXPECT_EQ(real.second, 0)
        << "a real decode must not increment the placeholder counter";

    EXPECT_EQ(gated.first, 255)
        << "with JBIG2 DISABLED the same fixture must come back blank";
    EXPECT_GT(gated.second, 0)
        << "with JBIG2 DISABLED the placeholder substitution must be recorded";

    EXPECT_NE(gated.first, real.first)
        << "the gated and ungated decodes are indistinguishable, so neither "
           "posture is actually being measured";
}

// The placeholder-overlay region set is non-empty when JBIG2 is disabled
// (default — the blank spot gets the notice) and empty when enabled (the real
// image is shown). Static, Document-based — no live widget needed.
TEST_F(ExternalFlawCorpusTest, Jbig2PlaceholderOverlay_GatedByPreference) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/jbig2-file-header.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);

    const bool saved = pdfe::core::isJbig2DecodingEnabled();

    pdfe::core::setJbig2DecodingEnabled(false);
    EXPECT_FALSE(pdfe::ui::PdfCanvas::jbig2PlaceholderRectsForPage(doc.get(), 0).isEmpty())
        << "disabled JBIG2 must yield a placeholder region over the hidden image";

    pdfe::core::setJbig2DecodingEnabled(true);
    EXPECT_TRUE(pdfe::ui::PdfCanvas::jbig2PlaceholderRectsForPage(doc.get(), 0).isEmpty())
        << "enabled JBIG2 shows the real image — no placeholder overlay";

    pdfe::core::setJbig2DecodingEnabled(saved);
}

// End-to-end at the shipping default (JBIG2 OFF): open + render the malformed
// fixture, document unmutated, the substitution recorded, and the render layer
// exposing a placeholder region over the hidden image.
TEST_F(ExternalFlawCorpusTest, Jbig2EndToEnd_DefaultOffRendersWithPlaceholder) {
    const bool saved = pdfe::core::isJbig2DecodingEnabled();
    pdfe::core::setJbig2DecodingEnabled(false);  // the shipping default

    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/jbig2-symbol-offset.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_FALSE(doc->isModified()) << "opening must not mutate the document";

    EXPECT_FALSE(pdfe::ui::PdfCanvas::jbig2PlaceholderRectsForPage(doc.get(), 0).isEmpty())
        << "the hidden JBIG2 image must get a placeholder region at the default (off)";

    // RASTERIZE, not merely build a display list: a display list records the
    // image reference and defers the decode, so a display-list-only check
    // cannot observe whether the decoder ran at all.
    const int before = pdfe::core::jbig2PlaceholderCount();
    const auto raster = rasterizePageZero("images/jbig2-symbol-offset.pdf");
    EXPECT_TRUE(raster.rendered || raster.boundedError)
        << "the malformed JBIG2 fixture must either rasterize or fail cleanly — "
           "and, either way, come back";
    EXPECT_GT(pdfe::core::jbig2PlaceholderCount(), before)
        << "rasterizing the page did not go through the placeholder, so the "
           "bytes went to jbig2dec instead — reaching this line proves only "
           "that nothing crashed on this run";

    pdfe::core::setJbig2DecodingEnabled(saved);
}

// CCITT Group-4 fax with EndOfBlock=false — decoder edge case.
TEST_F(ExternalFlawCorpusTest, CcittEndOfBlockFalse_OpenRendersGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/ccitt-endofblock-false.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// Positive control: an intact PDF/A-1b with a bitmap image renders cleanly.
TEST_F(ExternalFlawCorpusTest, HiResValidPdfA_OpenRenders) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/hires-valid.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_TRUE(renderPageZero(doc.get())) << "valid PDF/A image must render";
}

// Image XObject with a WRONG /Height vs the actual stream — the renderer must
// not trust the declared metadata into a crash (buffer over-read).
TEST_F(ExternalFlawCorpusTest, HiResWrongHeight_OpenRendersGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/hires-wrong-height.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); })
        << "a wrong declared /Height must not drive an over-read crash";
}

// Page content references a non-existent XObject ID (broken image reference).
TEST_F(ExternalFlawCorpusTest, HiResWrongObjectId_OpenRendersGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("images/hires-wrong-objectid.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); })
        << "a dangling XObject reference must be skipped, not crash";
}

// --- §6 Numeric edge cases -------------------------------------------------

// A real number exceeding the IEEE754 float range (and PDF/A limit) in
// coordinate math — must not produce NaN/inf-driven UB or a crash.
TEST_F(ExternalFlawCorpusTest, RealOverIeee754_OpenRendersGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("dimensions/real-over-ieee754-and-pdfa-limit.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// Real number below the PDF/A minimum, in document STRUCTURE.
TEST_F(ExternalFlawCorpusTest, RealUnderMinStructure_OpenRendersGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("dimensions/real-under-pdfa-min-structure.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// Same underflow class but in a CONTENT-STREAM operand (distinct code path).
TEST_F(ExternalFlawCorpusTest, RealUnderMinContentStream_OpenRendersGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("dimensions/real-under-pdfa-min-contentstream.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// >28 nested graphics-state q/Q operators — must be depth-bounded, not a
// stack-overflow (the interpreter DoS this class targets).
TEST_F(ExternalFlawCorpusTest, NestedQOver28_RendersBounded) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("dimensions/nested-q-over-28.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); })
        << "deeply-nested q/Q must be bounded, not overflow the gfx-state stack";
}

// --- §7 Transparency / blend / shading -------------------------------------

TEST_F(ExternalFlawCorpusTest, FunctionBasedShading_OpenRenders) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("vector/function-based-shading.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

TEST_F(ExternalFlawCorpusTest, KnockoutBlendMultiply_OpenRenders) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("vector/knockout-blend-multiply.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

TEST_F(ExternalFlawCorpusTest, BlendMode_OpenRenders) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("vector/blendmode.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

TEST_F(ExternalFlawCorpusTest, TransparencyGroup_OpenRenders) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("vector/transparency-group.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// --- §8 Degenerate annotations ---------------------------------------------

// Line annotation with an empty /Rect and no appearance stream (zero-area).
TEST_F(ExternalFlawCorpusTest, AnnotationEmptyRect_OpenRendersGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("overlap/annotation-empty-rect.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); })
        << "an empty-Rect annotation must not divide-by-zero / crash";
}

// Highlight annotation missing its appearance stream — renderer synthesizes one.
TEST_F(ExternalFlawCorpusTest, AnnotationHighlightNoAppearance_OpenRenders) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("overlap/annotation-highlight-no-appearance.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// --- §9 Embedded / attached content & scripting ----------------------------

// Document-level file attachment via the /EmbeddedFiles name dictionary.
TEST_F(ExternalFlawCorpusTest, FileAttachmentNameDict_OpenRenders) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("embedded/file-attachment-namedict.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
}

// Page-level file attachment via a File-Attachment ANNOTATION (distinct path).
TEST_F(ExternalFlawCorpusTest, FileAttachmentAnnotation_OpenRenders) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("embedded/file-attachment-annotation.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// SECURITY: a PDF with embedded JavaScript must NOT auto-execute it on open.
// PDFE runs JS only for explicit AcroForm field actions (src/forms/JavaScriptEngine);
// there is no document-open / OpenAction execution path. Assert opening +
// rendering the JS document neither crashes NOR mutates the document (an
// auto-run OpenAction script would be the observable side effect). This is the
// "safely ignore active content rather than execute it" property.
TEST_F(ExternalFlawCorpusTest, EmbeddedJavaScript_DoesNotAutoExecuteOnOpen) {
    // SEC-43 / GitHub #54 -- the original oracle was `!doc->isModified()`.
    //
    // That asserts the script did not mutate THE DOCUMENT. It says nothing
    // about the two things an auto-run script would most plausibly do instead:
    // start a process, or write a file. A payload that shelled out and touched
    // nothing in the PDF passed this test cleanly, which made a "does not
    // auto-execute" claim considerably narrower than it reads.
    //
    // Both spies are already in tests/security_oracles.h -- ProcessSpawnGuard
    // (which reads a Job completion port, so it also catches a child that has
    // already exited) and FileWriteGuard (SEC-44). The row asked for "a new
    // filesystem-write spy"; there was no need to write one.
    pdfe_test::ProcessSpawnGuard spawnGuard;

    const auto scratch = std::filesystem::temp_directory_path() /
                         "pdfe_sec43_js_scratch";
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);
    ASSERT_FALSE(ec) << "could not create the watched scratch directory";
    pdfe_test::FileWriteGuard writeGuard(scratch, "pdfe_sec43");

    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("embedded/javascript.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_FALSE(doc->isModified())
        << "opening a JS-bearing PDF must not auto-run its OpenAction script "
           "(no document mutation on open)";
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
    EXPECT_NO_THROW({ (void)doc->page(0)->extractPlainText(); });
    EXPECT_FALSE(doc->isModified())
        << "rendering/extracting a JS-bearing PDF must not run embedded JS";

    EXPECT_EQ(spawnGuard.newlySpawned(), "")
        << "opening/rendering a JS-bearing PDF spawned a process. The document "
           "was unmodified, so the original oracle would have called this a "
           "pass -- which is exactly the gap SEC-43 names.";
    EXPECT_FALSE(spawnGuard.observedNewInJobProcess())
        << "a process appeared in this test's job object and had already "
           "exited by the time it was checked -- a transient spawn is still a "
           "spawn";
    EXPECT_EQ(writeGuard.violation(), "")
        << "opening/rendering a JS-bearing PDF wrote to the filesystem";

    std::filesystem::remove_all(scratch, ec);
}

// --- §10 Forms (AcroForm / XFA) --------------------------------------------

TEST_F(ExternalFlawCorpusTest, AcroFormCalcOrder_OpenRenders) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("forms/acroform-calc-order.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// Filled XFA form (a completely different internal structure from AcroForm).
TEST_F(ExternalFlawCorpusTest, XfaFilled_OpenRendersGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("forms/xfa-filled.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// ===========================================================================
// §14  GRADED TRUNCATION CORPUS  (GitHub #29 / gap-register PDF-82)
// ===========================================================================
//
// The row asked for "new corrupt/ fixtures truncated at 25%/50%/75%/mid-xref,
// asserting graceful open-or-reject + recovered page count".
//
// WHY THE CORPUS IS GENERATED, NOT COMMITTED. Four truncations of one source
// are four near-identical binaries whose only interesting property is WHERE
// they were cut. Committing them hides that: a reader cannot see the cut
// point, and nothing keeps the "expected recovered pages" figure honest as the
// source changes. Generating them here means the source layout, the cut
// offsets and the expectation all come from ONE piece of arithmetic that a
// reader can check. Everything is written to a per-test temp directory and
// deleted on teardown; nothing resolves out of the gitignored external corpus,
// and a failure to BUILD the corpus is a hard failure, never a skip.
//
// WHY THE ORACLE IS NOT crash/no-crash. "It didn't crash" is satisfied in full
// by a parser that recovers NOTHING from every one of the four files — which
// is precisely the regression a truncation corpus exists to catch. So the
// oracle is the set of pages whose AUTHORED MARKER TEXT can still be
// extracted, compared against the set DERIVED from the cut offset:
//
//     page i is intact at cut C  <=>  the last byte of page i's content
//                                      object lies at or before C
//
// That set is computed from the generator's own recorded object offsets. It is
// not a number copied out of a first run. Two ways to fail it: recover fewer
// pages than are demonstrably still in the file (a parser that gives up early),
// or recover MORE (a parser inventing pages out of a /Count it can no longer
// justify — a real hazard, since /Pages /Count still says 12 in every cut).
//
// The one documented tolerance is the BOUNDARY page, whose content object
// straddles the cut. Whether a half-written content stream yields its text is
// genuinely underdetermined, so that single page — identified by arithmetic,
// not by observation — is permitted either way. Every other page is exact.
//
// MID-XREF IS THE DIFFERENT FAILURE MODE, and the reason it is called out
// separately in the row. Cutting inside the cross-reference table destroys the
// document's index while leaving every object byte intact. A parser that
// leans on the xref recovers nothing; a parser that reconstructs by scanning
// recovers EVERYTHING. There is no boundary page and no tolerance: the
// expectation is all 12 pages, exactly.

namespace graded_truncation {

constexpr int kPages = 12;

// Per-page filler, in the page's own content stream, as a PDF comment. Its
// only job is to space the pages far enough apart in the file that 25%, 50%
// and 75% land in three different pages — i.e. that the grades are genuinely
// graded rather than three names for the same damage.
constexpr int kFillerBytes = 900;

struct Corpus {
    std::string bytes;
    // Offset one past the final byte of page i's CONTENT object ("endobj\n").
    // Page i's own dictionary object is written immediately before it, so this
    // one offset covers everything page i needs.
    size_t contentEnd[kPages] = {0};
    // Page i's own dictionary object: [dictStart, dictEnd). dictEnd is also
    // where page i's content object begins. Recorded because a cut INSIDE a
    // dictionary and a cut inside a STREAM are different kinds of damage --
    // see the `cutInsidePageDict` derivation below.
    size_t dictStart[kPages] = {0};
    size_t dictEnd[kPages] = {0};
    char marker[kPages][32] = {{0}};
    size_t xrefStart = 0;   // offset of the "xref" keyword
    size_t xrefEnd = 0;     // offset one past the final xref entry
};

// Object numbering: 1 = catalog, 2 = page tree, then per page i:
// 3 + 2*i = page dict, 4 + 2*i = content stream.
inline int pageObjNum(int i)    { return 3 + 2 * i; }
inline int contentObjNum(int i) { return 4 + 2 * i; }
inline int totalObjs()          { return 3 + 2 * kPages; }

Corpus build() {
    Corpus g;
    std::string& out = g.bytes;
    std::vector<size_t> objOffset(static_cast<size_t>(totalObjs()), 0);

    out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";

    // 1 — catalog. FIRST in the file on purpose: every cut below keeps it, so
    // a reconstructing parser always has a /Type /Catalog to find.
    objOffset[1] = out.size();
    out += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    // 2 — page tree. /Count says 12 in every truncation, including the ones
    // where most of the kids are gone. That is deliberate: it is the trap a
    // parser falls into if it reports page count without checking that the
    // objects behind it still exist.
    objOffset[2] = out.size();
    out += "2 0 obj\n<< /Type /Pages /Count " + std::to_string(kPages) +
           " /Kids [";
    for (int i = 0; i < kPages; ++i) {
        if (i > 0) out += " ";
        out += std::to_string(pageObjNum(i)) + " 0 R";
    }
    out += "] >>\nendobj\n";

    // THE PAGE OBJECTS ARE WRITTEN OUT OF ORDER, on purpose.
    //
    // /Kids stays in reading order 0..11, but page i's objects are written to
    // the file at position (i*7 mod 12). Nothing about a PDF requires objects
    // to appear in page order, and real producers routinely do not.
    //
    // It matters here because it decides what a truncation can prove. If the
    // objects were in page order, the set of pages surviving any cut would be
    // a PREFIX {0..k} — and then a parser that filled a gap by substituting
    // some already-loaded page would land inside the surviving set every time,
    // and the soundness half of the oracle could never fail. Scrambling makes
    // the surviving set SCATTERED, so recovering a page whose bytes are gone
    // means naming a marker that is genuinely not in the file.
    for (int step = 0; step < kPages; ++step) {
        const int i = (step * 7) % kPages;

        std::snprintf(g.marker[i], sizeof(g.marker[i]), "PDFEMARKERP%02d", i);

        // The page dictionary.
        g.dictStart[i] = out.size();
        objOffset[static_cast<size_t>(pageObjNum(i))] = out.size();
        out += std::to_string(pageObjNum(i)) +
               " 0 obj\n<< /Type /Page /Parent 2 0 R "
               "/MediaBox [0 0 612 792] /Resources << /Font << /F1 << "
               "/Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >> "
               "/Contents " + std::to_string(contentObjNum(i)) +
               " 0 R >>\nendobj\n";

        // The content stream. The filler comes FIRST and the marker LAST, so
        // a stream cut part-way through loses its marker. That is what makes
        // "the marker extracted" mean "this page's content survived whole"
        // rather than "some of this page survived".
        std::string content;
        content.reserve(kFillerBytes + 128);
        for (int f = 0; f < kFillerBytes / 60; ++f) {
            content += "% graded truncation filler line, page ";
            content += std::to_string(i);
            content += "\n";
        }
        content += "BT\n/F1 18 Tf\n72 700 Td\n(";
        content += g.marker[i];
        content += ") Tj\nET\n";

        g.dictEnd[i] = out.size();
        objOffset[static_cast<size_t>(contentObjNum(i))] = out.size();
        out += std::to_string(contentObjNum(i)) + " 0 obj\n<< /Length " +
               std::to_string(content.size()) + " >>\nstream\n" + content +
               "endstream\nendobj\n";
        g.contentEnd[i] = out.size();
    }

    // A real cross-reference table, so the UNTRUNCATED control is a valid PDF
    // that opens through the xref rather than through repair.
    g.xrefStart = out.size();
    out += "xref\n0 " + std::to_string(totalObjs()) + "\n";
    out += "0000000000 65535 f \n";
    for (int n = 1; n < totalObjs(); ++n) {
        char entry[32];
        std::snprintf(entry, sizeof(entry), "%010llu 00000 n \n",
                      static_cast<unsigned long long>(objOffset[
                          static_cast<size_t>(n)]));
        out += entry;
    }
    g.xrefEnd = out.size();
    out += "trailer\n<< /Size " + std::to_string(totalObjs()) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(g.xrefStart) +
           "\n%%EOF\n";
    return g;
}

// A truncation grade: where to cut, and what that cut leaves behind.
struct Grade {
    const char* name;
    size_t cut;
    bool intact[kPages];      // derived: content object wholly below the cut
    int intactCount;
    int boundaryPage;         // page straddling the cut, or -1
    // TRUE when the cut lands INSIDE a page DICTIONARY, leaving a half-written
    // "<< /Type /Page /Parent 2 0 R /Media" as the last thing in the file.
    //
    // This distinction is derived from the offsets, not observed from a run,
    // and it is the difference between two genuinely different kinds of
    // damage. A cut inside a content STREAM leaves every object in the file
    // syntactically whole — the stream is simply short, and MuPDF's repair
    // recomputes /Length from what is there. A cut inside a DICTIONARY leaves
    // the file with an object that cannot be parsed at all; MuPDF's repair
    // scan (pdf_repair_xref) throws "invalid key in dict" on it and abandons
    // the whole document, so nothing is recovered even though earlier pages
    // are intact.
    //
    // The row this corpus implements asks for "graceful open-OR-REJECT", so a
    // clean rejection is a permitted answer — but ONLY here, and the oracle
    // says WHY in advance rather than excusing whatever it happens to see. If
    // the document does open at this grade, the completeness and soundness
    // assertions still apply in full.
    bool cutInsidePageDict;
};

Grade derive(const Corpus& g, const char* name, size_t cut) {
    Grade gr{};
    gr.name = name;
    gr.cut = cut;
    gr.intactCount = 0;
    gr.boundaryPage = -1;
    gr.cutInsidePageDict = false;
    for (int i = 0; i < kPages; ++i) {
        gr.intact[i] = (g.contentEnd[i] <= cut);
        if (gr.intact[i]) gr.intactCount++;
    }
    // The straddling page: whichever page's byte range [dictStart, contentEnd)
    // CONTAINS the cut. Found by offset, not by page index, because the page
    // objects are written to the file out of order (see build()).
    for (int i = 0; i < kPages; ++i) {
        if (cut > g.dictStart[i] && cut < g.contentEnd[i]) {
            gr.boundaryPage = i;
            // Inside the dictionary object, as opposed to inside the content
            // object that follows it.
            gr.cutInsidePageDict = (cut < g.dictEnd[i]);
            break;
        }
    }
    return gr;
}

}  // namespace graded_truncation

// Regression case from a filed XFA-handling bug.
TEST_F(ExternalFlawCorpusTest, XfaIssue14315_OpenRendersGracefully) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("forms/xfa-issue14315.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// Form with a reset-form ACTION — static open/render must not trip on it.
TEST_F(ExternalFlawCorpusTest, ResetFormAction_OpenRenders) {
    std::unique_ptr<pdfe::core::Document> doc;
    ASSERT_NO_THROW({ doc = openOrSkip("forms/resetform.pdf"); });
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    EXPECT_NO_THROW({ renderPageZero(doc.get()); });
}

// --- §14 Graded truncation corpus (GitHub #29 / PDF-82) ---------------------
// The design rationale is in the block comment at the head of §14 above.

namespace graded_truncation {

std::filesystem::path tempRoot() {
    const ::testing::TestInfo* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    auto d = std::filesystem::temp_directory_path() / "pdfe_graded_truncation"
             / (info != nullptr ? info->name() : "unknown");
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}

// Write the first `cut` bytes of `g.bytes` to `path`.
bool writeTruncated(const Corpus& g, const std::filesystem::path& path,
                    size_t cut) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f.write(g.bytes.data(), static_cast<std::streamsize>(
                                std::min(cut, g.bytes.size())));
    f.close();
    return std::filesystem::exists(path);
}

// Which authored markers can still be READ out of `path`. Returns the set of
// page indices whose marker text was extracted from SOME page of the document
// (a repaired document may renumber or re-order pages, so the set — not the
// position — is what is compared).
std::set<int> recoverMarkers(const Corpus& g,
                             const std::filesystem::path& path,
                             int* recoveredPageCountOut,
                             bool* threwOut = nullptr,
                             int* loadablePagesOut = nullptr) {
    std::set<int> found;
    *recoveredPageCountOut = -1;
    if (threwOut != nullptr) *threwOut = false;
    if (loadablePagesOut != nullptr) *loadablePagesOut = 0;
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    std::unique_ptr<pdfe::core::Document> doc;
    try {
        doc = pdfe::core::Document::open(ctx, path);
    } catch (const std::exception&) {
        // A rejection reported as a C++ exception is still a rejection, and
        // still graceful — but it is a DIFFERENT answer from a null return and
        // the caller is told which it got.
        if (threwOut != nullptr) *threwOut = true;
        return found;
    }
    if (!doc) return found;
    const int n = doc->pageCount();
    *recoveredPageCountOut = n;
    for (int p = 0; p < n; ++p) {
        std::string text;
        try {
            auto page = doc->page(p);
            if (!page) continue;
            if (loadablePagesOut != nullptr) ++(*loadablePagesOut);
            text = page->extractPlainText();
        } catch (const std::exception&) {
            continue;
        }
        for (int i = 0; i < kPages; ++i) {
            if (text.find(g.marker[i]) != std::string::npos) {
                found.insert(i);
            }
        }
    }
    return found;
}

}  // namespace graded_truncation

// The CONTROL, and the precondition every grade below depends on: the
// generated source is a valid 12-page PDF from which all 12 markers extract.
// Without this, "3 of 12 markers recovered at the 25% cut" could just as
// easily mean the generator emits an unreadable file, and every grade would be
// measuring the generator rather than the parser.
TEST_F(ExternalFlawCorpusTest, GradedTruncation_UntruncatedControlIsIntact) {
    using namespace graded_truncation;
    const Corpus g = build();
    const auto dir = tempRoot();
    const auto path = dir / "control.pdf";
    ASSERT_TRUE(writeTruncated(g, path, g.bytes.size()))
        << "could not write the generated corpus to " << path.string();

    int pages = -1;
    const std::set<int> found = recoverMarkers(g, path, &pages);
    ASSERT_EQ(pages, kPages)
        << "the UNTRUNCATED generated source does not open as a " << kPages
        << "-page PDF (got " << pages << "). Every expectation in the graded "
           "tests is derived from this file's layout, so nothing below means "
           "anything until this passes.";
    ASSERT_EQ(static_cast<int>(found.size()), kPages)
        << "only " << found.size() << " of " << kPages << " authored markers "
           "extract from the UNTRUNCATED source. The marker oracle used by "
           "every graded test would then under-report by construction.";

    // The grades must be genuinely GRADED — four distinct damage levels, not
    // four names for the same cut. Derived from the offsets, so a change to
    // the generator that collapsed them fails HERE rather than silently
    // turning three tests into copies of one.
    const size_t total = g.bytes.size();
    const Grade g25 = derive(g, "25%", total / 4);
    const Grade g50 = derive(g, "50%", total / 2);
    const Grade g75 = derive(g, "75%", (total * 3) / 4);
    const Grade gxr = derive(g, "mid-xref",
                             g.xrefStart + (g.xrefEnd - g.xrefStart) / 2);

    EXPECT_LT(g25.intactCount, g50.intactCount)
        << "the 25% and 50% cuts leave the same number of pages intact ("
        << g25.intactCount << "), so they are not distinct damage levels";
    EXPECT_LT(g50.intactCount, g75.intactCount)
        << "the 50% and 75% cuts leave the same number of pages intact ("
        << g50.intactCount << ")";
    EXPECT_LT(g75.intactCount, gxr.intactCount)
        << "the 75% cut and the mid-xref cut leave the same number of pages "
           "intact, so the mid-xref case is not testing the distinct "
           "index-destroyed-body-intact failure mode it exists for";
    EXPECT_EQ(gxr.intactCount, kPages)
        << "the mid-xref cut is supposed to damage ONLY the cross-reference "
           "table — every object byte precedes it. It left "
        << gxr.intactCount << " of " << kPages << " pages intact.";
    EXPECT_EQ(gxr.boundaryPage, -1)
        << "the mid-xref cut has a straddling page, which it must not: the cut "
           "is inside the xref table, past every object";

    // The completeness oracle is SUSPENDED for a grade whose cut lands inside
    // a page dictionary (see Grade::cutInsidePageDict). That escape hatch is
    // only honest while it stays rare: if a regeneration moved the objects so
    // that every grade landed in a dictionary, all four graded tests would
    // quietly stop asserting anything. Bound it here, from the derivation.
    const int lenient = (g25.cutInsidePageDict ? 1 : 0) +
                        (g50.cutInsidePageDict ? 1 : 0) +
                        (g75.cutInsidePageDict ? 1 : 0) +
                        (gxr.cutInsidePageDict ? 1 : 0);
    EXPECT_LE(lenient, 1)
        << lenient << " of the four grades cut inside a page DICTIONARY, so "
           "that many of them are permitted to answer 'rejected, recovered "
           "nothing'. The recovered-page oracle is only meaningful on the "
           "grades that cut inside a content STREAM; re-space the generator "
           "(kFillerBytes) so at most one grade lands in a dictionary. Do NOT "
           "widen the tolerance instead.";
    EXPECT_FALSE(gxr.cutInsidePageDict)
        << "the mid-xref grade must never take the lenient branch -- it cuts "
           "past every object";

    // The scramble must actually scramble: at the 50% cut the surviving pages
    // must NOT be a prefix {0..k}. If they were, the soundness half of the
    // oracle could not fail (see the comment in build()).
    {
        bool seenGap = false, seenAfterGap = false;
        for (int i = 0; i < kPages; ++i) {
            if (!g50.intact[i]) seenGap = true;
            else if (seenGap) seenAfterGap = true;
        }
        EXPECT_TRUE(seenAfterGap)
            << "at the 50% cut the surviving pages form an unbroken prefix, so "
               "the page objects are effectively in reading order and the "
               "soundness assertion in the graded tests cannot fail. The "
               "write-order permutation in build() has stopped permuting.";
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

namespace graded_truncation {

// The shared body. `allowRejection` says whether a clean refusal to open is an
// acceptable answer at this grade — it is, when the derivation says nothing is
// recoverable anyway, and it is NOT when whole pages are demonstrably intact.
void ExpectGradeRecoversDerivedPages(const char* gradeName, size_t cut) {
    const Corpus g = build();
    ASSERT_GT(cut, 0u);
    ASSERT_LT(cut, g.bytes.size())
        << "the '" << gradeName << "' cut is not a truncation at all";

    const Grade gr = derive(g, gradeName, cut);
    const auto dir = tempRoot();
    const auto path = dir / (std::string("truncated_") + "grade.pdf");
    ASSERT_TRUE(writeTruncated(g, path, cut));
    ASSERT_EQ(std::filesystem::file_size(path), cut);

    int pages = -1;
    int loadable = 0;
    bool threw = false;
    std::set<int> found;
    ASSERT_NO_THROW({
        found = recoverMarkers(g, path, &pages, &threw, &loadable);
    })
        << "opening the '" << gradeName << "' truncation escaped as an "
           "unhandled exception rather than failing cleanly";

    const bool rejected = (pages < 0);

    // ── Derived expectation.
    std::set<int> expected;
    for (int i = 0; i < kPages; ++i) if (gr.intact[i]) expected.insert(i);

    std::ostringstream ctxMsg;
    ctxMsg << "\n  grade      : " << gradeName
           << "\n  cut offset : " << cut << " of " << g.bytes.size()
           << "\n  derived intact pages : " << gr.intactCount
           << "\n  boundary (straddling) page : " << gr.boundaryPage
           << "\n  cut lands inside a page DICTIONARY : "
           << (gr.cutInsidePageDict ? "yes" : "no")
           << "\n  pages the parser reported  : " << pages
           << "\n  of those, actually loadable: " << loadable
           << "\n  markers recovered          : " << found.size();

    // (a) COMPLETENESS. Every page whose bytes are demonstrably all still in
    //     the file must come back. This is the assertion "recovers nothing"
    //     fails.
    //
    //     Suspended for exactly one derived case: a cut inside a page
    //     DICTIONARY leaves an unparseable object in the file, and MuPDF's
    //     repair scan abandons the whole document rather than salvaging the
    //     earlier ones. The row asks for "graceful open-OR-REJECT", so that
    //     rejection is permitted — but it is permitted BY THE DERIVATION, in
    //     advance, and only while the document really does refuse. If it
    //     opens, every page it claims is still checked below.
    const bool completenessSuspended = rejected && gr.cutInsidePageDict;
    if (!completenessSuspended) {
        for (int i : expected) {
            EXPECT_EQ(found.count(i), 1u)
                << "page " << i << " is WHOLLY INTACT in the truncated file "
                   "(its content object ends at byte " << g.contentEnd[i]
                << ", the cut is at " << cut << ") but its marker '"
                << g.marker[i] << "' could not be extracted. The parser gave "
                   "up on data that is still there." << ctxMsg.str();
        }
    } else {
        std::cout << "[ INFO     ] grade '" << gradeName << "': the cut falls "
                     "inside page " << gr.boundaryPage << "'s DICTIONARY "
                     "(bytes " << g.dictStart[gr.boundaryPage] << ".."
                  << g.dictEnd[gr.boundaryPage] << "), leaving a half-written "
                     "object that MuPDF's repair scan cannot parse. The "
                     "document was REJECTED cleanly (no crash, no hang, "
                  << (threw ? "clean throw" : "null return")
                  << "), which the PDF-82 row permits. NOTE the cost, which is "
                     "real and is recorded rather than waved away: "
                  << gr.intactCount << " whole pages are in the file and none "
                     "of them is recovered." << std::endl;
    }

    // (b) SOUNDNESS. Nothing may come back that is not in the file. /Pages
    //     /Count still claims 12 in every cut, so a parser that trusted it
    //     would fail here.
    for (int i : found) {
        const bool permitted = (expected.count(i) == 1) || (i == gr.boundaryPage);
        EXPECT_TRUE(permitted)
            << "the parser recovered page " << i << " ('" << g.marker[i]
            << "'), whose content object ends at byte " << g.contentEnd[i]
            << " -- past the cut at " << cut << ", so those bytes are NOT in "
               "the file. /Pages /Count still says " << kPages
            << " in this truncation; reporting a page on that basis is "
               "inventing recovery." << ctxMsg.str();
    }

    // (c) THE RECOVERED PAGE COUNT MUST MEAN SOMETHING. `/Pages /Count` still
    //     says 12 in every one of these files, and it is the easiest number to
    //     report — so the contract asserted here is that the count PDFE hands
    //     out is a count of pages it can actually deliver, not a number it
    //     read out of a dictionary whose objects are gone. Every page the
    //     document claims must load.
    if (pages >= 0) {
        EXPECT_EQ(loadable, pages)
            << "the document reports " << pages << " pages but only "
            << loadable << " of them can actually be loaded. A recovered-page "
               "count that promises pages page() cannot deliver is the "
               "/Pages /Count value passed through, not a recovery result."
            << ctxMsg.str();
        EXPECT_GE(pages, static_cast<int>(found.size()))
            << "the document reports " << pages << " pages but " << found.size()
            << " distinct page markers extracted from it" << ctxMsg.str();
        EXPECT_LE(pages, kPages)
            << "the truncated document reports MORE pages than the intact "
               "source had" << ctxMsg.str();
    }

    // (d) A grade with intact pages must not be REJECTED outright — unless
    //     the cut left an unparseable dictionary behind, per the derivation.
    if (gr.intactCount > 0 && !gr.cutInsidePageDict) {
        EXPECT_GE(pages, 0)
            << "the file still contains " << gr.intactCount
            << " whole pages, and every object in it is syntactically "
               "complete (the cut is inside a content STREAM, not a "
               "dictionary), yet the parser refused to open it at all. "
               "'Fails gracefully' is not the same as 'recovers nothing'."
            << ctxMsg.str();
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace graded_truncation

// 25% — the cut lands inside the front of the document body. Most page objects
// are simply absent.
TEST_F(ExternalFlawCorpusTest, GradedTruncation_At25Percent_RecoversDerivedPages) {
    using namespace graded_truncation;
    const Corpus g = build();
    ExpectGradeRecoversDerivedPages("25%", g.bytes.size() / 4);
}

// 50%.
TEST_F(ExternalFlawCorpusTest, GradedTruncation_At50Percent_RecoversDerivedPages) {
    using namespace graded_truncation;
    const Corpus g = build();
    ExpectGradeRecoversDerivedPages("50%", g.bytes.size() / 2);
}

// 75%.
TEST_F(ExternalFlawCorpusTest, GradedTruncation_At75Percent_RecoversDerivedPages) {
    using namespace graded_truncation;
    const Corpus g = build();
    ExpectGradeRecoversDerivedPages("75%", (g.bytes.size() * 3) / 4);
}

// MID-XREF — the distinct failure mode. Every object byte is present; only the
// cross-reference table (and the trailer behind it) is destroyed. A parser
// that reconstructs by scanning recovers all 12 pages; one that leans on the
// xref recovers none. There is no boundary page, so the expectation is exact.
TEST_F(ExternalFlawCorpusTest, GradedTruncation_MidXref_RecoversEveryPage) {
    using namespace graded_truncation;
    const Corpus g = build();
    const size_t cut = g.xrefStart + (g.xrefEnd - g.xrefStart) / 2;

    // Preconditions on the CUT itself, derived from the layout: it must be
    // inside the xref table and past every object.
    ASSERT_GT(cut, g.xrefStart);
    ASSERT_LT(cut, g.xrefEnd);
    ASSERT_LE(g.contentEnd[kPages - 1], g.xrefStart)
        << "the last page's content object runs past the start of the xref "
           "table, so this cut is not the index-only damage it claims to be";

    ExpectGradeRecoversDerivedPages("mid-xref", cut);

    // And the exact half, which the shared body deliberately states as a
    // bound rather than an equality.
    const auto dir = tempRoot();
    const auto path = dir / "midxref_exact.pdf";
    ASSERT_TRUE(writeTruncated(g, path, cut));
    int pages = -1;
    const std::set<int> found = recoverMarkers(g, path, &pages);
    EXPECT_EQ(static_cast<int>(found.size()), kPages)
        << "cutting inside the cross-reference table left every object byte in "
           "the file, so a reconstructing parser must recover ALL " << kPages
        << " pages. It recovered " << found.size()
        << ". This is the failure mode a content-stream truncation cannot "
           "show: the body is whole and only the index is gone.";
    EXPECT_EQ(pages, kPages)
        << "the reported page count after an index-only truncation is "
        << pages << ", not " << kPages;

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace
