#include "Document.h"
#include "Page.h"
#include "MuPdfContext.h"
#include "PathSecurity.h"
#include "core/ZoneIdentifier.h"
#include "AtomicFileReplace.h"
#include "BookmarkExtractor.h"
#include "PageArtifactGraft.h"
#include "FormXObjectDepthGuard.h"
#include "PdfSignatureProbe.h"
#include "BookmarkNode.h"
#include "Error.h"
// GitHub #100 / WIN-78 — advisory free-space pre-check. Advisory is the whole
// contract: it warns and returns, so the hardened stage-and-rename path (and
// the ENOSPC data-loss guard that exercises it) is unchanged.
#include "SaveSpacePrecheck.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>
#include <chrono>

// R2-04 (data-loss): atomic in-place save needs a real replace-existing rename
// plus an fsync of the temp. On Windows that means MoveFileExW / FlushFileBuffers;
// on POSIX, rename() + fsync(). Keep the platform headers narrow.
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>      // _chsize_s / _fileno — the staging output's truncate
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace pdfe::core {

// PDFE-LIMITATION-UNICODE-PATH fix: MuPDF's C API (fz_open_document,
// pdf_save_document) takes a narrow `const char*` path. On Windows,
// `std::filesystem::path::string()` converts the native wide path to the
// system ANSI code page, which silently drops any character not in that
// code page (CJK, Cyrillic, Arabic, even Latin-1 "café"). MuPDF on Windows
// internally converts a UTF-8 path to UTF-16 via fz_utf8_to_wchar before
// calling the wide Win32 file API, so we must hand it UTF-8 — NOT the ANSI
// narrow string. `path::u8string()` yields the lossless UTF-8 form on every
// platform; on POSIX it is byte-identical to `string()`.
static std::string mupdfPathUtf8(const std::filesystem::path& p)
{
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// R2-04 (data-loss): flush a just-written file to stable storage before we
// rename it over the original, so a crash immediately after the rename cannot
// leave the directory entry pointing at unflushed data. Best-effort — a failure
// here does NOT abort the save (the rename still yields a complete file, we only
// forfeit the durability guarantee) — but it is attempted on every save.
static void flushFileToStableStorage(const std::filesystem::path& p) noexcept
{
#if defined(_WIN32)
    // Reopen the temp (pdf_save_document already wrote + closed it) and flush its
    // buffers to disk. FILE_SHARE_READ so a concurrent reader cannot deadlock us.
    HANDLE h = ::CreateFileW(
        p.native().c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;  // best-effort
    }
    ::FlushFileBuffers(h);
    ::CloseHandle(h);
#else
    const std::string s = p.string();
    int fd = ::open(s.c_str(), O_RDWR);
    if (fd < 0) {
        return;  // best-effort
    }
    ::fsync(fd);
    ::close(fd);
#endif
}

// ---------------------------------------------------------------------------
// WIN-87 (phase-56) — the full rewrite, written THROUGH the staging handle.
// ---------------------------------------------------------------------------
//
// WHAT THIS REPLACES, AND WHY THE OLD SHAPE WAS EXPLOITABLE. save()'s
// full-rewrite branch and saveAs() used to hand pdf_save_document a PATH. That
// resolved "<target>.pdfe_tmp" three separate times — the write, the
// path-based reopen inside flushFileToStableStorage, and the rename — with no
// handle held on it at any point in between. Every one of those three is a
// nameable moment: the same shape phase-52 demonstrated end-to-end against the
// staged incremental append, where a concurrent unprivileged process stamped
// its own bytes into the staged file and PDFE atomically published them over
// the user's document and reported success. Fixing only the rename would have
// left two of the three open, and a test that attacked only the rename window
// would still have gone green.
//
// THE ROUTE. Identical in shape to saveIncrementalStaged()'s: MuPDF is handed a
// DUPLICATE of StagingFile's exclusive handle as a stdio stream
// (_open_osfhandle/_fdopen → fz_new_output_with_file_ptr → pdf_write_document),
// so it never opens the staging file by name; the flush goes through the
// exclusive handle; and publication is a rename through that same handle. From
// creation to publication there is no instant at which the staged bytes are
// openable for writing or deletion by anyone else.
//
// THAT SENTENCE IS ABOUT INTEGRITY, AND ONLY INTEGRITY -- write and delete, not
// read. It is exactly true and was still easy to read as a confidentiality
// guarantee it does not make (F71). Confidentiality is now held too, but by a
// DIFFERENT mechanism worth knowing about separately: StagingFile applies the
// DESTINATION's DACL at CREATION rather than at publish, so the staging file
// does not spend the append wearing the containing directory's inherited ACEs.
// FILE_SHARE_READ remains deliberate, so the file IS readable -- by whoever the
// destination's own ACL admits, which is the point.
//
// pdf_write_document IS pdf_save_document MINUS THE PATH, FOR THIS CALLER —
// verified in MuPDF 1.26.10 source/pdf/pdf-write.c, not assumed. Both run the
// same argument checks and both end in do_pdf_save_document(); the two
// differences are (a) pdf_save_document opens the output itself via
// fz_new_output_with_path, which is precisely what we are removing, and (b)
// pdf_save_document runs a do_appearance synthesis pass (:2734-2760) that
// pdf_write_document does not — irrelevant here because neither of these two
// call sites sets opts.do_appearance (both zero-initialise).
//
// AND `bias` IS NOT A CONCERN ON THIS ROUTE — THE OPPOSITE OF THE INCREMENTAL
// CASE. pdf-write.c:2763 sets `opts.bias = doc->bias` only under
// `if (in_opts->do_incremental)`. A full rewrite never sets do_incremental, so
// opts.bias is 0 in pdf_save_document and pdf_write_document alike, and the two
// emit byte-identical output for a document with a header preamble. The
// header-preamble compensation saveIncrementalStaged() carries — and its
// post-write truncateCalls/biasActive assertion, and its refusal of
// bias != 0 plus unsaved signatures — are therefore CORRECT ONLY on the
// incremental route and MUST NOT be extended to this one: a full rewrite of a
// biased document works today and would start being refused.
static void writeFullRewriteThroughStagingHandle(const MuPdfContext& ctx,
                                                 pdf_document* pdfDoc,
                                                 StagingFile& staging,
                                                 const pdf_write_options& opts)
{
    std::error_code ec;
    std::FILE* fp = staging.openDuplicateStream(ec);
    if (!fp) {
        throw PdfeException(
            "Could not open the staging stream for the save (target left "
            "intact): " + ec.message());
    }

    // Ownership of `fp` passes to MuPDF here: file_drop fcloses it and
    // fz_drop_output calls that — on the throwing path too, including the case
    // where fz_new_output itself fails (output.c: fz_new_output calls the drop
    // function before rethrowing). Nothing below may fclose it again. The
    // DUPLICATE dying costs us nothing: StagingFile's own exclusive handle is
    // untouched and still spans the whole window, creation to publication.
    ctx.execute([pdfDoc, fp, &opts](fz_context* c) {
        fz_output* out = NULL;
        fz_var(out);
        // No C++ objects with destructors are constructed inside this fz_try
        // region — MuPDF's setjmp/longjmp would not unwind them.
        fz_try(c) {
            out = fz_new_output_with_file_ptr(c, fp);
            pdf_write_document(c, pdfDoc, out, &opts);
            fz_close_output(c, out);
        }
        fz_always(c) {
            fz_drop_output(c, out);
        }
        fz_catch(c) {
            fz_rethrow(c);
        }
    });
}

// F58-d (phase-53). Is a "<original>.pdfe_tmp" orphan a COMPLETE, finished save
// — the only thing the R2-04 recovery contract may offer a user — or a truncated
// carcass? A finished pdf_save_document always ends with `startxref\n<n>\n%%EOF`,
// so the test is: it starts with a `%PDF-` header, it has a `startxref`, and a
// `%%EOF` follows the LAST one.
//
// WHAT THIS DELIBERATELY IS NOT. The review also proposed rejecting an orphan
// whose size equals the original's. That heuristic is unsound, and an existing
// control proves it: a full-rewrite temp of an unedited document legitimately
// comes out the same size, and
// DocumentSaveAtomicTest.OrphanTempRecoveryDetectedWithoutClobber stages exactly
// that case as a COMPLETE orphan. Size carries no information about
// completeness; the trailer does.
//
// AND IT DOES NOT GATE SURFACING — see the constructor. Two pre-existing controls
// (that one and SaveFailureInjectionTest.TruncatedOrphanTempIsSurfacedNotApplied)
// require EVERY orphan to be surfaced, truncated ones included, so residue is
// shown to the user rather than silently hidden. Suppressing them would have been
// a weakened test, not a fix. The real defect was the false PROMISE attached to
// the surfaced path; the fix is to publish this verdict alongside it, via
// Document::pendingRecoveredTempIsComplete().
static bool orphanTempLooksComplete(const std::filesystem::path& temp)
{
    std::error_code ec;
    const auto tempSize = std::filesystem::file_size(temp, ec);
    if (ec || tempSize < 32) {
        return false;
    }

    std::ifstream in(temp, std::ios::binary);
    if (!in) {
        return false;
    }
    char head[16] = {0};
    in.read(head, sizeof(head));
    if (in.gcount() < 5 || std::memcmp(head, "%PDF-", 5) != 0) {
        return false;
    }

    // The tail carries both markers for any sane save; 4 KiB is generous.
    const std::uintmax_t tailBytes =
        std::min<std::uintmax_t>(tempSize, 4096u);
    std::vector<char> tail(static_cast<std::size_t>(tailBytes));
    in.clear();
    in.seekg(static_cast<std::streamoff>(tempSize - tailBytes), std::ios::beg);
    in.read(tail.data(), static_cast<std::streamsize>(tailBytes));
    if (static_cast<std::uintmax_t>(in.gcount()) != tailBytes) {
        return false;
    }
    const std::string t(tail.data(), tail.size());
    const auto sx = t.rfind("startxref");
    if (sx == std::string::npos) {
        return false;
    }
    return t.find("%%EOF", sx) != std::string::npos;
}

// F58-a (phase-53) had a static stagedPrefixMatchesOriginal() here: it
// re-read the first `originalSize` bytes of the staging file through the
// re-acquired exclusive handle and compared them with the original, to close
// the window the `bias != 0` by-path write opened.
//
// DELETED BY F62 (phase-55), together with the window it was closing — and the
// reason it is worth a tombstone is that it LOOKED like a sound closure and was
// not. Its loop ran `while (offset < originalSize)`: an upper bound captured
// BEFORE the operation it was verifying had written anything. So it covered the
// pre-existing prefix and could not, by construction, observe
// `[originalSize, end)` — the appended tail, which is both what the by-path
// write had just produced and what an attacker holding a write handle would
// have forged. A LARGER forged tail passed it, passed the must-not-shrink
// guard, and would have been published over the user's document. Nothing was
// wrong with the comparison inside the loop; the BOUND was the defect.
//
// It is not replaced by a wider version. There is no longer any interval in
// which the staged bytes can change, so there is nothing to re-establish. See
// DCA 0020 and the BiasedStagingOutput block below.

// ---------------------------------------------------------------------------
// F62 (phase-55) — THE BIAS-COMPENSATING STAGING OUTPUT.
//
// WHY THIS EXISTS AT ALL. Until phase-55 saveIncrementalStaged() had two routes.
// Documents with `pdf_document::bias == 0` were appended to THROUGH the staging
// file's exclusive handle. Documents with garbage before their `%PDF-` marker
// (bias != 0) could not be, because `pdf_save_document` sets the private
// `opts.bias = doc->bias` and `pdf_write_document` does not — so those took a
// fallback that RELEASED the exclusive handle, let `pdf_save_document` write the
// staging file BY PATH, re-acquired the handle, and re-verified.
//
// THE DEFECT (F62). That re-verification — stagedPrefixMatchesOriginal() — loops
// `while (offset < originalSize)`. It covers the pre-existing PREFIX. It never
// looks at `[originalSize, end)`, which is exactly the region pdf_save_document
// had just written and exactly the region exposed while no handle was held. The
// only other check was a must-not-shrink guard, and a forged tail is strictly
// LARGER, so it passed. A verification routine whose bounds exclude the bytes
// the operation just produced cannot fail for the reason it exists.
//
// WHY THE TAIL WAS NOT SIMPLY HASHED INSTEAD (the remedy the backlog row itself
// proposed). Hashing the tail "as written" needs an output-side tee, and
// `pdf_save_document` opens the file BY PATH — there is no fz_output to wrap. A
// digest taken after re-acquisition proves only self-consistency. So the tail
// could not be honestly verified; the window had to go instead.
//
// WHY IT COULD GO — the MuPDF facts, verified in 1.26.10's own source, not
// assumed:
//   * `bias` is a member of the PRIVATE `pdf_write_state` (pdf-write.c:73), NOT
//     of the public `pdf_write_options` (mupdf/pdf/document.h). It cannot be
//     set from outside. `pdf_save_document` assigns it at pdf-write.c:2764;
//     `pdf_write_document` never does.
//   * It is consumed at exactly four number-EMITTING sites: `ofs_list[num] -
//     opts->bias` in writexrefsubsect and in writexrefstreamsubsect, and
//     `startxref - opts->bias` (both the emitted literal and `doc->startxref`)
//     in writexref and writexrefstream. Every other byte of the output is
//     bias-independent. Its whole job is to turn PHYSICAL output offsets into
//     offsets RELATIVE to `%PDF-`, which is the coordinate system a biased
//     file's xref is written in.
//   * So the same correction can be applied from outside by making the OUTPUT
//     report positions relative to `%PDF-`: then `ofs_list` and `startxref` are
//     already relative and `- opts->bias` (with opts->bias == 0) is the
//     identity.
//
// AND WHY IT CANNOT BE A UNIFORM SHIFT — the trap that had to be avoided.
// `do_pdf_save_document` calls `ensure_initial_incremental_contents(ctx,
// doc->file, opts->out, doc->file_size)` BEFORE anything else. That function
// compares the output's existing bytes against the original IN PHYSICAL
// COORDINATES (via fz_stream_from_output), and then does
// `fz_seek_output(out, len, SEEK_SET)` with `len == doc->file_size`, also
// physical, followed by `fz_truncate_output`. An output shifted by `bias`
// throughout would compare the wrong bytes, decide the staged copy did not
// match, and rewrite the whole prefix at the wrong offset.
//
// THE BOUNDARY, AND WHY IT IS TRUSTWORTHY. So the output starts PHYSICAL and
// flips to RELATIVE at one point: the `fz_truncate_output` call. In pdf-write.c
// `fz_truncate_output` appears at exactly two lines, 2108 and 2132, and both are
// inside `ensure_initial_incremental_contents` — one per branch, so exactly ONE
// truncate happens per incremental write, and it happens precisely when the
// output has been positioned at the end of the original prefix and everything
// still to come is the appended revision. That is the boundary we need.
//
// THE FLIP IS NOT TRUSTED, IT IS CHECKED. Coupling to a library's internal call
// pattern is a real hazard, so it is made fail-closed rather than silent: the
// truncate count and the physical position at the flip are both recorded, the
// position is required to equal the original prefix length, and
// saveIncrementalStaged() refuses to publish unless the flip happened exactly
// once. A MuPDF upgrade that changed the call pattern therefore produces a
// refused save with a diagnosable message, never a document whose xref offsets
// are silently wrong by `bias`. `StagedIncrementalBiasTest.
// BiasedAppendMatchesPdfSaveDocumentByteForByte` pins the output against
// pdf_save_document's for the same document, so a drift is a red test and not a
// judgement call.
//
// THE ONE THING THAT STILL CANNOT BE DONE FROM OUTSIDE, and it is WORSE than a
// wrong /ByteRange (corrected by the phase-55 security review — the earlier text
// here understated it, and anyone re-evaluating "can we just fix the ByteRange?"
// from that description would have underestimated the hazard).
//
// When a signature is CREATED in this same save (`pdf_has_unsaved_sigs`), MuPDF
// computes `usig->byte_range_start` as `bstr - buf + opts->ofs_list[pnum]`. With
// the biased output installed, `ofs_list` is DEFLATED by bias while the offset
// within the serialised object is not, and the two CANCEL: `byte_range_start`
// comes out in PHYSICAL coordinates. It is then fed straight back to
// `fz_seek_output(..., SEEK_SET)`, and biasedOutSeek ADDS bias to a SEEK_SET —
// so the /ByteRange patch, and `pdf_write_digest`'s seek to the /Contents
// placeholder, both land `bias` bytes PAST where they belong.
//
// The failure is therefore not "a /ByteRange that resolves against the wrong
// bytes". It is OVERWRITING ARBITRARY BYTES OF THE APPENDED REVISION with the
// byte-range array and the signature blob. One external output cannot serve both
// coordinate systems; only the private `opts.bias` can, because it applies the
// correction at the emission sites rather than at the output.
// saveIncrementalStaged() therefore REFUSES that combination (bias != 0 AND
// unsaved signatures) with a diagnosable error, and the refusal is not a
// close call.
//
// NET EFFECT: there is no longer any route through this method that lets go of
// the staging file's exclusive handle. StagingFile::releaseHandleForExternalWrite
// and ::reacquireExclusiveHandle are DELETED rather than left unused, so no
// future caller can reintroduce the window by reaching for them. An exposure
// window that does not exist needs no verification.
// ---------------------------------------------------------------------------
namespace {

/// Caller-owned state for the bias-compensating fz_output. It deliberately
/// OUTLIVES the fz_output: the drop callback closes the FILE* (MuPDF's ownership
/// contract) but does NOT free this, so saveIncrementalStaged() can inspect
/// `truncateCalls` / `biasActive` after the write and refuse to publish if the
/// boundary did not land where it must.
struct BiasedStagingOutput {
    std::FILE* fp = nullptr;
    std::int64_t bias = 0;           ///< 0 ⇒ an exact pass-through
    std::int64_t prefixLength = 0;   ///< physical size of the original prefix
    int truncateCalls = 0;
    bool biasActive = false;
};

inline std::int64_t stagingTell(std::FILE* fp)
{
#if defined(_WIN32)
    return ::_ftelli64(fp);
#else
    return static_cast<std::int64_t>(::ftello(fp));
#endif
}

inline int stagingSeek(std::FILE* fp, std::int64_t off, int whence)
{
#if defined(_WIN32)
    return ::_fseeki64(fp, off, whence);
#else
    return ::fseeko(fp, static_cast<off_t>(off), whence);
#endif
}

// NOTE (MuPDF hazard): every one of these runs inside MuPDF's setjmp/longjmp
// fz_try regions. None of them may construct a C++ object with a destructor and
// none may throw a C++ exception — failures leave by fz_throw.
void biasedOutWrite(fz_context* ctx, void* state, const void* data, size_t n)
{
    auto* s = static_cast<BiasedStagingOutput*>(state);
    if (n == 0) {
        return;
    }
    if (std::fwrite(data, 1, n, s->fp) != n) {
        fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot write to the staging file");
    }
}

void biasedOutSeek(fz_context* ctx, void* state, int64_t off, int whence)
{
    auto* s = static_cast<BiasedStagingOutput*>(state);
    // Only SEEK_SET carries a coordinate; SEEK_CUR/SEEK_END are relative to a
    // physical position and are identical in both coordinate systems.
    const std::int64_t target =
        (whence == SEEK_SET && s->biasActive) ? off + s->bias : off;
    if (stagingSeek(s->fp, target, whence) != 0) {
        fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot seek in the staging file");
    }
}

int64_t biasedOutTell(fz_context* ctx, void* state)
{
    auto* s = static_cast<BiasedStagingOutput*>(state);
    const std::int64_t off = stagingTell(s->fp);
    if (off < 0) {
        fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot tell in the staging file");
    }
    return s->biasActive ? off - s->bias : off;
}

fz_stream* biasedOutAsStream(fz_context* ctx, void* state)
{
    auto* s = static_cast<BiasedStagingOutput*>(state);
    std::fflush(s->fp);
    // PHYSICAL, deliberately. Its only live consumer is
    // ensure_initial_incremental_contents, which runs BEFORE the flip and
    // compares against doc->file in physical coordinates. (Its other consumer,
    // the unsaved-signature fixup, cannot be reached with bias != 0 — that
    // combination is refused before the write starts.)
    return fz_open_file_ptr_no_close(ctx, s->fp);
}

void biasedOutTruncate(fz_context* ctx, void* state)
{
    auto* s = static_cast<BiasedStagingOutput*>(state);
    std::fflush(s->fp);
    const std::int64_t pos = stagingTell(s->fp);
    if (pos < 0) {
        fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot tell in the staging file");
    }
#if defined(_WIN32)
    if (::_chsize_s(::_fileno(s->fp), pos) != 0) {
        fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot truncate the staging file");
    }
#else
    if (::ftruncate(::fileno(s->fp), static_cast<off_t>(pos)) != 0) {
        fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot truncate the staging file");
    }
#endif

    // THE BOUNDARY. See the block comment above: this is the one moment at which
    // the prefix is complete and everything still to be written belongs to the
    // appended revision. Fail closed on anything unexpected — a second truncate,
    // or one at a position that is not the end of the original prefix, means the
    // assumption this compensation rests on no longer holds, and a save that
    // refuses is the only acceptable outcome.
    ++s->truncateCalls;
    if (s->bias != 0) {
        if (s->truncateCalls != 1) {
            fz_throw(ctx, FZ_ERROR_GENERIC,
                     "staged incremental save: the writer truncated the staging "
                     "file more than once, so the bias boundary is not where "
                     "this build assumes — refusing to produce xref offsets that "
                     "cannot be vouched for");
        }
        if (pos != s->prefixLength) {
            // fz_throw's formatter does not implement %lld — it emits the
            // directive verbatim, which is how this message was first shipped
            // reading "truncated ... at %ld, not at ... %ld". Pre-format into a
            // plain char buffer (no destructor, so it is safe across the
            // setjmp boundary) and hand fz_throw a %s.
            char detail[320];
            std::snprintf(detail, sizeof(detail),
                          "staged incremental save: refused because the size "
                          "MuPDF believes this file has (%lld) is not where "
                          "the append boundary landed (%lld). Those two must "
                          "agree for the header-preamble offset correction to "
                          "be placed correctly, so nothing is published and "
                          "the original is left intact. This is NOT a defect "
                          "in MuPDF's writer. It means MuPDF's idea of the "
                          "file and the file itself have diverged since it was "
                          "opened, which happens two ways: another writer "
                          "appended to the document, or a journal snapshot was "
                          "restored in this session. Reopening the document "
                          "and retrying resolves both.",
                          static_cast<long long>(pos),
                          static_cast<long long>(s->prefixLength));
            fz_throw(ctx, FZ_ERROR_GENERIC, "%s", detail);
        }
        s->biasActive = true;
    }
}

void biasedOutDrop(fz_context* /*ctx*/, void* state)
{
    auto* s = static_cast<BiasedStagingOutput*>(state);
    // MuPDF's contract: fz_drop_output owns and closes the stream, on the
    // throwing path too (fz_new_output calls drop before rethrowing). The state
    // struct itself belongs to the caller and is NOT freed here.
    if (s->fp) {
        std::fclose(s->fp);
        s->fp = nullptr;
    }
}

}  // namespace

// R2-04 (data-loss): atomically replace @p to with @p from on the same volume,
// NEVER remove-first. On success the destination becomes the new file; on ANY
// failure the destination (the user's original) is left completely intact — there
// is no window in which the original is absent.
//
// F10/WIN-85: the implementation now lives in core/AtomicFileReplace.{h,cpp} so
// BatchEngine / ImageExporter / RedactionManager use the SAME code (including the
// bounded retry), rather than four divergent bare renames. Declared in the same
// namespace (pdfe::core), so the call sites below are unchanged.

// Tier 2 (pipeline b2e47d91, phase-3): the do_garbage level a full-rewrite
// save uses when the session contains NO dedup-worthy operation. gc=1 removes
// unreferenced objects (so orphaned/redacted bytes are still scrubbed) but
// skips the super-linear dedup pass (gc=3) that made large-XFA saves take
// minutes. Phase-1 benchmark (tests/docs/xfa_save_benchmark.md) confirmed gc=1
// round-trips cleanly on all three XFA fixtures at 150–980× the speed of gc=3.
static constexpr int kDefaultFullRewriteGc = 1;

class Document::Impl {
public:
    std::shared_ptr<MuPdfContext> context;
    fz_document* doc = nullptr;
    pdf_document* pdfDoc = nullptr;
    std::filesystem::path filePath;
    int cachedPageCount = 0;
    bool modified = false;
    // F14 (PDF-01): the authored `%PDF-x.y` header version, captured from the
    // raw bytes at open time. MuPDF clobbers pdf_document::version with the
    // catalog /Version, so this is the only surviving record of the header.
    // Invalid (major==0) for in-memory / header-less documents.
    PdfVersion headerVersion_{};
    // F14 (PDF-01): an explicit floor set by setMinimumPdfVersion(). The
    // full-rewrite save paths raise the emitted header to at least
    // max(headerVersion_, catalog /Version, versionFloor_) so no save can
    // downgrade the document.
    PdfVersion versionFloor_{};
    // R3-04 autosave (Phase-2): bumped by markContentChanged() /
    // markRequiresFullRewrite() / markNeedsFullGc() on every content edit,
    // so AutosaveManager can detect edits that push no undo command. Never
    // reset by the save paths (they clear `modified`, not the version).
    uint64_t contentVersion_ = 0;

    // Flip the dirty bit AND bump the content version in one place, so every
    // content-mutating edit is visible both to isModified() and to autosave.
    void markContentChanged() {
        modified = true;
        ++contentVersion_;
    }
    // R2-12: actual authentication level captured at open (0=none,1=user,
    // 2=owner,3=both). Defaults to owner-equivalent (2) so unencrypted / non-PDF
    // documents — which carry no access restrictions — never spuriously block a
    // privileged operation. Overwritten for encrypted documents by the real
    // fz_authenticate_password result of the password actually supplied.
    int authLevel = 2;
    mutable std::mutex docMutex;  // Serializes fz_document access from worker threads
    AnnotationFilter annotFilter_ = AnnotationFilter::ShowAll;
    std::set<int> dirtyPageIndices_;  // Pages with modified content streams

    // ISSUE #125: real subscribers for markPageDirty(). A VECTOR of
    // (token, callback), not a map keyed by callback — std::function is not
    // comparable, and registration order is the delivery order, which is worth
    // being deterministic about. Guarded by docMutex; see markPageDirty() for
    // why the callbacks are invoked with that lock released.
    std::vector<std::pair<Document::PageDirtyToken, Document::PageDirtyListener>>
        pageDirtyListeners_;
    Document::PageDirtyToken nextPageDirtyToken_ = 0;

    // Destroyed with this Impl, i.e. with the Document. Observers weak_ptr it
    // to tell a live Document from a freed one — see Document::lifetimeSentinel().
    std::shared_ptr<const void> lifetime_ = std::make_shared<const int>(0);
    Document::DocumentEventCallback eventCb_;  // Pipeline E phase-9

    // Tier 1 incremental-save routing (pipeline b2e47d91).
    //  requiresFullRewrite_: sticky — a destructive/structural edit this
    //    session means the next save must go through the full-rewrite path
    //    (do_garbage=3) so orphaned/removed bytes are scrubbed.
    //  incrementalSafe_: true iff the in-memory object numbering still
    //    matches the on-disk file at `filePath`. True after open() and after
    //    a full-rewrite save() that reopens; false after saveAs() (which
    //    renumbers via GC but does NOT reopen, so an incremental append would
    //    corrupt the file).
    bool requiresFullRewrite_ = false;
    bool incrementalSafe_ = true;
    // F24 — message from the most recent DocMDP save-guard verdict on this
    // document. See Document::runSaveGuard.
    std::string lastSaveGuardWarning_;
    // Tier 2 (pipeline b2e47d91, phase-3): narrower than requiresFullRewrite_.
    // Set only by dedup-worthy ops (redaction, page delete/reorder/import,
    // flatten, font embed, encryption); selects do_garbage=3 over the cheaper
    // default in the full-rewrite save paths. See effectiveGarbageLevel().
    bool needsFullGc_ = false;

    // GitHub #19 / PDF-50: what a full-rewrite save may do to this document's
    // applied signatures. See Document::SignedRewritePolicy.
    Document::SignedRewritePolicy signedRewritePolicy_ =
        Document::SignedRewritePolicy::Refuse;

    // GitHub #19 / PDF-50: applied signatures counted ONCE, at open time. See
    // the capture site in open() for why it cannot be counted at save time.
    int appliedSignaturesAtOpen_ = 0;

    // PDF-02 (meta-audit gap register, issue #1): did the document, AS OPENED,
    // store any object inside an object stream? Captured ONCE at open time,
    // deliberately: an edit rewrites the touched objects into the incremental
    // xref as plain in-use entries, so a scan performed at save time would
    // report progressively fewer 'o' entries as a session went on, and a
    // heavily-edited document would silently drop the shape. The question the
    // save paths need answered is about the file that arrived, not about what
    // is left of it.
    bool sourceUsesObjectStreams_ = false;

    // R2-04 (data-loss) recovery: set at open time when a sibling
    // "<filePath>.pdfe_tmp" is found — a fully-written new version a crashed
    // prior save never renamed over the original. Surfaced (never auto-applied)
    // via Document::pendingRecoveredTemp() so a caller/UI can offer recovery.
    std::optional<std::filesystem::path> pendingRecoveredTemp_;
    // F58-d: whether that orphan is a FINISHED save or a truncated carcass. The
    // R2-04 contract asserted the former in prose with nothing checking it.
    bool pendingRecoveredTempComplete_ = false;

    // #46: a scratch/bake temp file this Document was reopened ONTO and now
    // holds open — e.g. RedactionManager's rewriteDocument=true path bakes the
    // redaction via save("<orig>.redact_tmp"), which reopens the live document
    // onto that temp (the reopen is required to actually drop the redacted
    // bytes). The still-open document holds it, so no external caller can remove
    // it. We own that lifecycle: delete it once we drop our own handle (dtor, or
    // a later reopen onto a different file). The temp holds POST-redaction
    // (scrubbed) content, so this is disk hygiene, not a confidentiality issue.
    std::filesystem::path scratchTempToDelete_;

    // WIN-87 phase-57. A staging file this Document has been REOPENED ONTO
    // because a publish could not land — see saveThroughExternalRewrite's
    // failure branch for why that is the correct fallback rather than reopening
    // the un-rewritten original. The StagingFile object is retained, not just
    // its path, so its exclusive handle stays open for as long as we read from
    // it. Its destructor deletes the file when the Document dies, so it is
    // never left behind.
    std::unique_ptr<StagingFile> retainedRewriteStaging_;

    // The DUPLICATE stream MuPDF reads that staging file through. It is a
    // duplicate of the handle above rather than a fresh open BY NAME, because a
    // by-name open cannot succeed at all: the StagingFile reserves
    // FILE_SHARE_READ, the CRT asks for share read+write, and PDFE would deny
    // itself access to its own file. Retained because MuPDF reads lazily and
    // fz_open_file_ptr_no_close does not take ownership — so this must outlive
    // the fz_document and be closed with it, which releaseRetainedRewrite()
    // below does in the one place that can get the order right.
    std::FILE* retainedRewriteStream_ = nullptr;

    /// Drop the retained bake stream and staging file together, in that order:
    /// the stream is a duplicate of the staging file's handle, so closing it
    /// first keeps the staging file's own handle valid until its destructor
    /// runs. Safe to call when nothing is retained.
    void releaseRetainedRewrite() noexcept {
        if (retainedRewriteStream_) {
            std::fclose(retainedRewriteStream_);
            retainedRewriteStream_ = nullptr;
        }
        retainedRewriteStaging_.reset();
    }

    Impl(std::shared_ptr<MuPdfContext> ctx, const std::filesystem::path& path)
        : context(std::move(ctx)), filePath(path) {}

    void fireEvent(const std::string& name) noexcept {
        if (eventCb_) {
            try { eventCb_(name); } catch (...) { /* swallow — event hooks must not break save/close */ }
        }
    }

    ~Impl() {
        // Pipeline E phase-9: fire willClose before MuPDF tears down the document.
        fireEvent("willClose");
        if (doc && context) {
            context->execute([this](fz_context* ctx) {
                fz_drop_document(ctx, doc);
            });
            doc = nullptr;
        }
        // WIN-87 phase-57. The fz_document that read through the retained bake
        // stream has just been dropped, so this is the first moment it is safe
        // to close that stream — and it must happen HERE rather than by member
        // destruction, because it is a raw FILE* (fz_open_file_ptr_no_close
        // does not take ownership). Closing it before the staging file's own
        // handle is released is the required order; releaseRetainedRewrite()
        // owns that ordering so it is not restated in two places.
        releaseRetainedRewrite();

        // #46: our handle is now released — delete any scratch/bake temp we were
        // holding open (e.g. a redaction .redact_tmp). Non-throwing; a
        // destructor must never throw.
        if (!scratchTempToDelete_.empty()) {
            std::error_code ec;
            std::filesystem::remove(scratchTempToDelete_, ec);
        }
    }
};

// meta-audit follow-up F14 (PDF-01). Read the `%PDF-x.y` header version
// straight from the file's leading bytes.
//
// Why not ask MuPDF: pdf_document::version is overwritten with the catalog
// /Version whenever one is present, so the authored header value is gone by
// the time the document is open. ISO 32000-1 §7.5.5 makes the catalog override
// conditional (it applies only when LATER), which cannot be evaluated without
// the header value — hence this reader.
//
// The header must be at byte 0 per §7.5.2, but files with leading junk are
// common and MuPDF's repair path accepts them, so scan the same 1024-byte
// window _looks_like_pdf() uses and take the FIRST `%PDF-` marker.
static std::optional<PdfVersion> _read_header_version(
    const std::filesystem::path& filePath)
{
    constexpr std::size_t kScanBytes = 1024;
    std::ifstream f(filePath, std::ios::binary);
    if (!f) return std::nullopt;
    char buf[kScanBytes] = {0};
    f.read(buf, kScanBytes);
    const std::streamsize n = f.gcount();
    for (std::streamsize i = 0; i + 8 <= n; ++i) {
        if (buf[i  ] == '%' && buf[i+1] == 'P' && buf[i+2] == 'D'
         && buf[i+3] == 'F' && buf[i+4] == '-') {
            // "%PDF-" + at least "x.y"
            const std::streamsize avail = n - (i + 5);
            const std::size_t take = static_cast<std::size_t>(
                avail < 8 ? avail : 8);
            return PdfVersion::parse(std::string_view(buf + i + 5, take));
        }
    }
    return std::nullopt;
}

Document::Document(
    std::shared_ptr<MuPdfContext> context,
    const std::filesystem::path& filePath,
    const std::string& password
)
    : pImpl(std::make_unique<Impl>(std::move(context), filePath))
{
    // Open the document. Use UTF-8 (not the ANSI narrow string) so non-ASCII
    // paths reach MuPDF intact — see mupdfPathUtf8 / PDFE-LIMITATION-UNICODE-PATH.
    std::string pathStr = mupdfPathUtf8(filePath);
    const char* passwordPtr = password.empty() ? nullptr : password.c_str();

    // Open document first (no C++ exceptions inside fz_try/fz_catch).
    //
    // NOTE on hostile input: a cyclic / pathologically-nested PDF can drive
    // MuPDF's parser/repair into unbounded recursion that exhausts the stack.
    // That crash CANNOT be trapped here — fz_try/fz_catch is setjmp-based and
    // a stack overflow also corrupts the allocator. The application guards
    // against it OUT OF PROCESS, before reaching this constructor: the UI runs
    // an isolated `pdfe_preflight` child process (see core/PreflightOpen and
    // app/pdfe_preflight.cpp) and refuses any document that child cannot open
    // cleanly. This raw open therefore assumes a pre-vetted file.
    pImpl->doc = pImpl->context->executeReturn<fz_document*>(
        [&pathStr](fz_context* ctx) -> fz_document* {
            return fz_open_document(ctx, pathStr.c_str());
        }
    );

    // Check password outside fz_try/fz_catch (C++ exceptions safe here)
    bool needsPwd = pImpl->context->executeReturn<bool>(
        [this](fz_context* ctx) -> bool {
            return fz_needs_password(ctx, pImpl->doc) != 0;
        }
    );

    if (needsPwd) {
        if (!passwordPtr) {
            pImpl->context->execute([this](fz_context* ctx) {
                fz_drop_document(ctx, pImpl->doc);
            });
            pImpl->doc = nullptr;
            throw PdfeException("Password required to open this document");
        }

        int auth = pImpl->context->executeReturn<int>(
            [this, passwordPtr](fz_context* ctx) -> int {
                return fz_authenticate_password(ctx, pImpl->doc, passwordPtr);
            }
        );

        if (!auth) {
            pImpl->context->execute([this](fz_context* ctx) {
                fz_drop_document(ctx, pImpl->doc);
            });
            pImpl->doc = nullptr;
            throw PdfeException("Invalid password");
        }
    }

    // Get PDF-specific document pointer if this is a PDF
    pImpl->pdfDoc = pImpl->context->executeReturn<pdf_document*>(
        [this](fz_context* ctx) -> pdf_document* {
            return pdf_specifics(ctx, pImpl->doc);
        }
    );

    // Cache page count
    pImpl->cachedPageCount = pImpl->context->executeReturn<int>(
        [this](fz_context* ctx) -> int {
            return fz_count_pages(ctx, pImpl->doc);
        }
    );

    // F14 (PDF-01): capture the authored %PDF-x.y header version NOW. MuPDF has
    // already overwritten pdf_document::version with the catalog /Version (if
    // any), so the header must be read from the file's own bytes.
    if (auto hv = _read_header_version(filePath)) {
        pImpl->headerVersion_ = *hv;
    }

    // PDF-02: capture whether this document arrived using object streams, so a
    // full-rewrite save can PRESERVE that shape instead of expanding every
    // bundled object back out into a free-standing one. See
    // Document::sourceUsesObjectStreams().
    if (pImpl->pdfDoc) {
        pImpl->sourceUsesObjectStreams_ = pImpl->context->executeReturn<bool>(
            [this](fz_context* ctx) -> bool {
                const int n = pdf_count_objects(ctx, pImpl->pdfDoc);
                for (int i = 1; i < n; ++i) {
                    // _no_change: never solidifies the xref and never throws,
                    // so scanning a document costs nothing and cannot perturb
                    // the state the rest of open() is about to observe. It
                    // returns NULL for slots that are not present.
                    pdf_xref_entry* e =
                        pdf_get_xref_entry_no_change(ctx, pImpl->pdfDoc, i);
                    if (e && e->type == 'o') return true;
                }
                return false;
            }
        );
    }

    // GitHub #19 / PDF-50: how many APPLIED signatures did this document have
    // when it arrived?
    //
    // CAPTURED AT OPEN, AND THAT IS THE WHOLE POINT. The save-time guard
    // cannot ask the live document, because the operation it exists to stop
    // has already DESTROYED THE EVIDENCE by then: `FormManager::flattenForm`
    // calls `pdf_bake_document(bake_widgets=1)`, which orphans the signature
    // widget and leaves `/Root /AcroForm` pointing at a dead object, so a
    // save-time count returns 0 and the guard sails through on precisely the
    // path that did the most damage. Measured, not theorised — the first
    // version of this guard counted at save time, refused the annotation
    // flatten correctly, and let the form flatten publish a signature-free
    // file exactly as before.
    if (pImpl->pdfDoc) {
        pImpl->appliedSignaturesAtOpen_ = pImpl->context->executeReturn<int>(
            [this](fz_context* ctx) -> int {
                return pdfe::core::countAppliedSignatures(ctx, pImpl->pdfDoc);
            });
    }

    // R2-12: capture the REAL authentication level (owner vs user) from MuPDF,
    // keyed on the password actually supplied, rather than inferring owner-ship
    // from the permission bitmask (which mislabels a fully-permissioned user as
    // owner). Re-authenticating with the SAME password that opened the document
    // is idempotent — it re-validates without changing the established access.
    // Unencrypted / non-PDF documents keep the owner-equivalent default (2).
    {
        const std::string effectivePw = password;  // copy across setjmp boundary
        pImpl->authLevel = pImpl->context->executeReturn<int>(
            [this, effectivePw](fz_context* ctx) -> int {
                if (!pImpl->pdfDoc) return 2;  // non-PDF: nothing to restrict
                pdf_obj* trailer = pdf_trailer(ctx, pImpl->pdfDoc);
                pdf_obj* enc = pdf_dict_get(ctx, trailer, PDF_NAME(Encrypt));
                if (!enc) return 2;  // unencrypted: owner-equivalent authority
                // Normalize MuPDF's raw bitfield into this class's convention
                // (1=user, 2=owner, 3=both). MuPDF 1.26 fz_authenticate_password
                // returns: 0=fail, 1=no crypt, 0x2=user-password match,
                // 0x4=owner-password match, 0x6=both. Owner is bit 0x4 — NOT the
                // low bit — so the raw value must be re-mapped, not stored as-is.
                const int raw = fz_authenticate_password(
                    ctx, pImpl->doc, effectivePw.c_str());
                if (raw == 0) return 0;   // authentication failed
                if (raw == 1) return 2;   // no crypt (defensive): owner-equivalent
                int lvl = 0;
                if (raw & 2) lvl |= 1;    // user-password bit -> user
                if (raw & 4) lvl |= 2;    // owner-password bit -> owner
                return lvl;
            }
        );
    }

    // R2-04 (data-loss) orphaned-temp recovery scan. A crashed prior save may
    // have written the new version to "<filePath>.pdfe_tmp" and died before the
    // atomic rename-over (Document::save step 4).
    //
    // F58-d (phase-53) — WHAT THIS COMMENT USED TO CLAIM, AND WHY IT WAS FALSE.
    // It said the temp "is a COMPLETE, pdf_save_document-finished file that
    // never replaced the original", and recorded ANY regular file at that path
    // with no validation whatsoever — not size, not `%PDF-`, not `startxref`.
    // Phase-52's staged incremental append then began writing to the same
    // suffix, and a real TerminateProcess mid-append left a 46 MB residue whose
    // only `startxref` is the ORIGINAL's: it opens as the PRE-save document with
    // 46 MB of unsigned junk past the last `%%EOF`, which for a signed document
    // is the classic incremental-update tampering shape. Offering that to a user
    // as "your unsaved changes" is the trap.
    //
    // TWO CHANGES CLOSE IT:
    //   * staged incremental appends no longer use this suffix at all. They use
    //     core::StagingFile's "<filePath>.pdfe_part.<hex>", so "complete
    //     rewrite" and "possibly-truncated append" are distinguishable BY NAME
    //     and this scan can no longer confuse one for the other. Those residues
    //     are swept below rather than surfaced — they are internal artifacts
    //     that are never a recoverable document.
    //   * what remains — a genuine full-rewrite temp — is VALIDATED before it is
    //     recorded: it must be a non-empty regular file, distinct in size from
    //     the original, and structurally finished (a trailing `%%EOF` after the
    //     last `startxref`). A truncated one is left on disk but not offered.
    if (!pImpl->filePath.empty()) {
        // The COMPLETE-full-rewrite shape specifically (F58-d). Deliberately
        // NOT a generic "any staging residue" match: the ".pdfe_part.<hex>"
        // shape is swept below, never surfaced as recoverable.
        const std::filesystem::path orphanTemp =
            fullRewriteTempPathFor(pImpl->filePath);
        std::error_code oec;
        if (std::filesystem::exists(orphanTemp, oec)
            && std::filesystem::is_regular_file(orphanTemp, oec)
            ) {
            pImpl->pendingRecoveredTemp_ = orphanTemp;
            pImpl->pendingRecoveredTempComplete_ =
                orphanTempLooksComplete(orphanTemp);
        }

        // Sweep our own staged-append residue. Unambiguously ours (the opened
        // file's full name + ".pdfe_part." + 32 hex digits), never user data,
        // and never recoverable. A CONCURRENT save's staging file is protected
        // from this by its own exclusive handle: remove() simply fails, which is
        // the correct outcome.
        const auto dir = pImpl->filePath.parent_path();
        const auto stem = pImpl->filePath.filename();
        std::error_code dirEc;
        for (std::filesystem::directory_iterator it(
                 dir.empty() ? std::filesystem::path(".") : dir, dirEc), end;
             !dirEc && it != end; it.increment(dirEc)) {
            if (StagingFile::isStagingResidueName(stem, it->path().filename())) {
                std::error_code rmEc;
                std::filesystem::remove(it->path(), rmEc);  // best-effort
            }
        }
    }
}

Document::~Document() = default;

Document::Document(Document&&) noexcept = default;

Document& Document::operator=(Document&&) noexcept = default;

// Refuse files that are not PDFs by magic bytes. Per ISO 32000-2 §7.5.2,
// a PDF MUST start with `%PDF-` within the first 1024 bytes (the PDF
// spec allows a small header preamble for legacy MIME-encoded files,
// but the canonical magic is `%PDF-` at offset 0). We scan a small
// window so SMTP/MIME-wrapped PDFs continue to work, while polyglots
// that disguise SVG/XML/HTML as a `.pdf` extension are refused.
//
// Why this matters: phase-3 corpus runs found that a malicious SVG
// polyglot triggered Qt's QtSvg renderer (via MuPDF's SVG handler),
// which in turn caused Qt to do hostname resolution for embedded
// `xlink:href` URLs -- visible in pfirewall.log as NetBIOS / LLMNR
// broadcast attempts attributed to PDFE.exe. Refusing non-PDF
// inputs at the boundary closes that attack vector entirely.
static bool _looks_like_pdf(const std::filesystem::path& filePath)
{
    constexpr std::size_t kScanBytes = 1024;
    std::ifstream f(filePath, std::ios::binary);
    if (!f) return false;
    char buf[kScanBytes] = {0};
    f.read(buf, kScanBytes);
    const std::streamsize n = f.gcount();
    // Look for `%PDF-` (0x25 0x50 0x44 0x46 0x2D) in the scanned window.
    for (std::streamsize i = 0; i + 5 <= n; ++i) {
        if (buf[i  ] == '%' && buf[i+1] == 'P' && buf[i+2] == 'D'
         && buf[i+3] == 'F' && buf[i+4] == '-') {
            return true;
        }
    }
    return false;
}

std::string PdfVersion::toString() const {
    if (!isValid()) return {};
    return std::to_string(major) + "." + std::to_string(minor);
}

std::optional<PdfVersion> PdfVersion::parse(std::string_view text) {
    // Tolerate a leading "%PDF-" so callers can hand over a raw header line.
    constexpr std::string_view kMarker = "%PDF-";
    if (text.size() >= kMarker.size() && text.substr(0, kMarker.size()) == kMarker) {
        text.remove_prefix(kMarker.size());
    }
    std::size_t i = 0;
    int major = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        if (major > 99) return std::nullopt;   // absurd — reject rather than wrap
        major = major * 10 + (text[i] - '0');
        ++i;
    }
    if (i == 0 || i >= text.size() || text[i] != '.') return std::nullopt;
    ++i;
    std::size_t minorStart = i;
    int minor = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        if (minor > 99) return std::nullopt;
        minor = minor * 10 + (text[i] - '0');
        ++i;
    }
    if (i == minorStart) return std::nullopt;
    PdfVersion v{major, minor};
    if (!v.isValid()) return std::nullopt;
    return v;
}

std::unique_ptr<Document> Document::open(
    std::shared_ptr<MuPdfContext> context,
    const std::filesystem::path& filePath,
    const std::string& password,
    OpenOrigin origin
)
{
    if (!context) {
        throw PdfeException("MuPdfContext is null");
    }

    // Refuse UNC / network-scheme inputs before touching the filesystem.
    // The kernel resolver fires NetBIOS/LLMNR/mDNS broadcasts the
    // moment a UNC path is canonicalized, even by a benign call like
    // std::filesystem::exists. Phase-3 corpus runs attributed those
    // broadcasts to PDFE.exe; refusing the path here closes the source.
    // Pass the path to the UNC/remote check losslessly. `.string()` on
    // Windows throws std::system_error ("No mapping for the Unicode
    // character...") for any char not in the system ANSI code page (e.g.
    // CJK), so use the wide overload on Windows and the UTF-8 form for
    // exception messages (PDFE-LIMITATION-UNICODE-PATH).
#ifdef _WIN32
    const bool isRemote = path_security::is_unc_or_remote(filePath.wstring());
#else
    const bool isRemote = path_security::is_unc_or_remote(filePath.string());
#endif

    if (isRemote && origin != OpenOrigin::UserInitiated) {
        // GitHub #240 — refused because the DOCUMENT asked, not because the path
        // is remote.
        //
        // A person naming a file on their own file server is ordinary. A PDF
        // naming a UNC resource is asking this process to reach a host the
        // document chose, which is the SMB / NetBIOS egress vector GitHub #91
        // (WIN-60) closed, and that stays closed.
        //
        // The default is DocumentRequested, so a caller that says nothing lands
        // here. That is deliberate: the permissive direction has to be typed.
        // Names UNC explicitly. PathRoundTrips.UncPathOpenRefusedByPolicy
        // requires the refusal to identify the UNC / network cause, and it is
        // right to: a message that says only "refused" leaves a user and a log
        // reader guessing which of several policies fired.
        throw PdfeException(
            "Refusing UNC / network path requested by document content (a user "
            "may open network files directly; a document may not reach them): "
            + mupdfPathUtf8(filePath));
    }

    if (!std::filesystem::exists(filePath)) {
        throw PdfeException("File does not exist: " + mupdfPathUtf8(filePath));
    }

    if (!_looks_like_pdf(filePath)) {
        throw PdfeException(
            "Not a PDF file (magic bytes %PDF- missing in first 1024 bytes): "
            + mupdfPathUtf8(filePath));
    }

    return std::unique_ptr<Document>(
        new Document(std::move(context), filePath, password)
    );
}

std::unique_ptr<Document> Document::createBlank(
    std::shared_ptr<MuPdfContext> context,
    const std::filesystem::path& outputPath,
    float widthPt,
    float heightPt
)
{
    if (!context) {
        throw PdfeException("MuPdfContext is null");
    }

    // Write a minimal blank PDF via MuPDF and then reopen it through
    // the normal Document::open path so the Document gets the full
    // bookkeeping (pImpl initialization, page count cache, etc.).
    std::string outputStr = mupdfPathUtf8(outputPath);
    context->execute([&](fz_context* ctx) {
        pdf_document* newDoc = pdf_create_document(ctx);
        if (!newDoc) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "pdf_create_document failed");
        }
        fz_try(ctx) {
            fz_rect mediabox = fz_make_rect(0, 0, widthPt, heightPt);
            pdf_obj* pageObj = pdf_add_page(ctx, newDoc, mediabox, 0,
                                             nullptr, nullptr);
            pdf_insert_page(ctx, newDoc, 0, pageObj);
            pdf_drop_obj(ctx, pageObj);

            pdf_write_options opts{};
            // A fresh single-page blank has nothing to de-duplicate; use the
            // cheap default GC level (pipeline b2e47d91, Tier 2).
            opts.do_garbage = kDefaultFullRewriteGc;
            opts.do_compress = 1;
            pdf_save_document(ctx, newDoc, outputStr.c_str(), &opts);
        } fz_always(ctx) {
            pdf_drop_document(ctx, newDoc);
        } fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });

    // A freshly created blank is not yet a stable on-disk original we can
    // append to, so force its first save through the full-rewrite path.
    // After that save reopens the file, it transitions to incremental-
    // eligible like any normally-opened document (see Document::save).
    auto doc = Document::open(context, outputPath);
    doc->markRequiresFullRewrite();
    return doc;
}

// F58-f (phase-53). pageCount()/page()/pageInfo() are the three places a NULL
// pImpl->doc becomes a null dereference rather than an error. The window is real:
// saveIncrementalStaged() (and save()) necessarily drop the fz_document before
// the rename, and the reopen afterwards can genuinely fail — an A/V scanner or
// the search indexer holding a sharing violation on a just-renamed file is
// routine on Windows. Before this, pageCount() returned the STALE cached int and
// page(i) range-checked against it and then called fz_load_page(ctx, nullptr, i).
// drainRebindAroundWrite does exactly pageCount() then page(idx).
//
// Fail closed: no live document ⇒ zero pages, and any page request is an error
// with a message that says why.
int Document::pageCount() const {
    if (!pImpl->doc) {
        return 0;
    }
    return pImpl->cachedPageCount;
}

PageInfo Document::pageInfo(int index) const {
    if (!pImpl->doc) {
        throw PdfeException(
            "Document has no open handle (a save dropped it and the re-open "
            "failed) — cannot read page " + std::to_string(index));
    }
    if (index < 0 || index >= pImpl->cachedPageCount) {
        throw PdfeException("Page index out of range: " + std::to_string(index));
    }

    std::lock_guard<std::mutex> lock(pImpl->docMutex);

    return pImpl->context->executeReturn<PageInfo>(
        [this, index](fz_context* ctx) -> PageInfo {
            // Load the page temporarily to get its info.
            //
            // GUARDED, and this is the site the first version of the #159 fix
            // MISSED. pageInfo() is reached by simply OPENING a document —
            // PagePanel builds a thumbnail for every page, PdfCanvas asks for
            // the current one, MainWindow asks for page 0 — so leaving it
            // unguarded left the whole "open a hostile file and the process
            // dies" threat model intact while page() and createDisplayList()
            // looked fixed.
            fz_page* page = safeLoadPage(ctx, pImpl->doc, index);

            PageInfo info;
            info.index = index;

            // Get page bounds
            fz_rect bounds = fz_bound_page(ctx, page);
            info.widthPt = bounds.x1 - bounds.x0;
            info.heightPt = bounds.y1 - bounds.y0;

            // Get rotation (0, 90, 180, 270)
            if (pImpl->pdfDoc) {
                pdf_page* pdfPage = pdf_page_from_fz_page(ctx, page);
                if (pdfPage) {
                    pdf_obj* rotate = pdf_dict_get(ctx, pdfPage->obj, PDF_NAME(Rotate));
                    if (rotate) {
                        info.rotation = pdf_to_int(ctx, rotate);
                    }
                }
            }

            // Get page label
            char labelBuf[256];
            fz_page_label(ctx, page, labelBuf, sizeof(labelBuf));
            info.label = labelBuf;

            // Drop the page
            fz_drop_page(ctx, page);

            return info;
        }
    );
}

// PDF-85 / GitHub #32 + #159. Every page load in this file goes through
// pdfe::core::safeLoadPage(), which refuses an over-deep Form XObject nest
// before MuPDF's recursive resource walk starts. See
// src/core/FormXObjectDepthGuard.h — including the note on why a local
// per-call-site helper (which is what this file had first, and which covered
// two of the three sites below) was replaced by a shared loader.

std::unique_ptr<Page> Document::page(int index) {
    // F58-f — see the note above pageCount(). Fail closed BEFORE the range
    // check, because with a null handle the cached bound is itself meaningless.
    if (!pImpl->doc) {
        throw PdfeException(
            "Document has no open handle (a save dropped it and the re-open "
            "failed) — cannot load page " + std::to_string(index));
    }
    if (index < 0 || index >= pImpl->cachedPageCount) {
        throw PdfeException("Page index out of range: " + std::to_string(index));
    }

    std::lock_guard<std::mutex> lock(pImpl->docMutex);

    fz_page* fzPage = pImpl->context->executeReturn<fz_page*>(
        [this, index](fz_context* ctx) -> fz_page* {
            return safeLoadPage(ctx, pImpl->doc, index);
        }
    );

    pdf_page* pdfPage = nullptr;
    if (pImpl->pdfDoc) {
        pdfPage = pImpl->context->executeReturn<pdf_page*>(
            [this, fzPage](fz_context* ctx) -> pdf_page* {
                return pdf_page_from_fz_page(ctx, fzPage);
            }
        );
    }

    return std::unique_ptr<Page>(
        new Page(pImpl->context, fzPage, pdfPage, index, this)
    );
}

bool Document::isModified() const {
    return pImpl->modified;
}

// F62 (phase-55). See Document.h for why this is observable at all.
int Document::preambleBias() const {
    if (!pImpl || !pImpl->pdfDoc) {
        return 0;
    }
    return pImpl->context->executeReturn<int>(
        [this](fz_context*) -> int { return pImpl->pdfDoc->bias; });
}

// F62 (phase-55). See Document.h for why this is observable at all.
bool Document::hasUnsavedSignatures() const {
    if (!pImpl || !pImpl->pdfDoc) {
        return false;
    }
    return pImpl->context->executeReturn<bool>(
        [this](fz_context* ctx) -> bool {
            return pdf_has_unsaved_sigs(ctx, pImpl->pdfDoc) != 0;
        });
}

uint64_t Document::contentVersion() const {
    return pImpl->contentVersion_;
}

void Document::markModified() {
    pImpl->markContentChanged();
}

bool Document::isPdf() const {
    return pImpl->pdfDoc != nullptr;
}

// ---------------------------------------------------------------------------
// PDF version handling — meta-audit follow-up F14 (PDF-01 / PDF-07 / PDF-08).
// ---------------------------------------------------------------------------

PdfVersion Document::headerVersion() const {
    return pImpl->headerVersion_;
}

std::optional<PdfVersion> Document::catalogVersion() const {
    if (!pImpl->pdfDoc) return std::nullopt;
    // Return the encoded key (major*10+minor), or 0 when absent/malformed, so
    // nothing but an int crosses the setjmp boundary.
    const int key = pImpl->context->executeReturn<int>(
        [this](fz_context* ctx) -> int {
            pdf_obj* root = pdf_dict_get(
                ctx, pdf_trailer(ctx, pImpl->pdfDoc), PDF_NAME(Root));
            if (!root) return 0;
            pdf_obj* ver = pdf_dict_get(ctx, root, PDF_NAME(Version));
            if (!ver || !pdf_is_name(ctx, ver)) return 0;
            const char* name = pdf_to_name(ctx, ver);
            if (!name) return 0;
            // Hand-parse here: PdfVersion::parse builds no RAII state, but the
            // string_view it needs would point at MuPDF-owned memory that must
            // not outlive this frame, so decode to the int key immediately.
            int major = 0, minor = 0, i = 0;
            for (; name[i] >= '0' && name[i] <= '9'; ++i) {
                if (major > 99) return 0;
                major = major * 10 + (name[i] - '0');
            }
            if (i == 0 || name[i] != '.') return 0;
            ++i;
            const int minorStart = i;
            for (; name[i] >= '0' && name[i] <= '9'; ++i) {
                if (minor > 99) return 0;
                minor = minor * 10 + (name[i] - '0');
            }
            if (i == minorStart || name[i] != '\0') return 0;
            if (major <= 0) return 0;
            return major * 10 + minor;
        }
    );
    if (key <= 0) return std::nullopt;
    return PdfVersion{key / 10, key % 10};
}

PdfVersion Document::pdfVersion() const {
    PdfVersion effective = pImpl->headerVersion_;
    // ISO 32000-1 §7.5.5: the catalog /Version overrides the header ONLY when
    // it is LATER. MuPDF applies it unconditionally; PDFE does not.
    if (auto cv = catalogVersion()) {
        if (!effective.isValid() || *cv > effective) effective = *cv;
    }
    if (pImpl->versionFloor_.isValid() && pImpl->versionFloor_ > effective) {
        effective = pImpl->versionFloor_;
    }
    if (!effective.isValid() && pImpl->pdfDoc) {
        // Header-less / in-memory document: fall back to whatever MuPDF holds.
        const int k = pImpl->pdfDoc->version;
        if (k > 0) effective = PdfVersion{k / 10, k % 10};
    }
    return effective;
}

void Document::setMinimumPdfVersion(PdfVersion v) {
    if (!pImpl->pdfDoc) {
        throw PdfeException("setMinimumPdfVersion: not a PDF document");
    }
    if (!v.isValid()) {
        throw PdfeException("setMinimumPdfVersion: invalid version");
    }
    const PdfVersion current = pdfVersion();
    if (current.isValid() && v <= current) return;   // never lowers

    pImpl->versionFloor_ = v;

    // The emitted header carries the version on the next full-rewrite save
    // (see applyVersionFloorForSave). Additionally record it in the catalog
    // when the on-disk header cannot express it — an incremental append never
    // rewrites the header, so `/Version` is the only channel there.
    const int key = v.key();
    pImpl->context->execute([this, key](fz_context* ctx) {
        pdf_obj* root = pdf_dict_get(
            ctx, pdf_trailer(ctx, pImpl->pdfDoc), PDF_NAME(Root));
        if (!root) {
            fz_throw(ctx, FZ_ERROR_GENERIC,
                     "setMinimumPdfVersion: document has no /Root");
        }
        char name[8];
        name[0] = static_cast<char>('0' + (key / 10));
        name[1] = '.';
        name[2] = static_cast<char>('0' + (key % 10));
        name[3] = '\0';
        pdf_dict_put_name(ctx, root, PDF_NAME(Version), name);
    });
    pImpl->markContentChanged();
}

// F14 (PDF-01): raise MuPDF's header version to the document's effective
// version immediately before a full-rewrite save.
//
// WHY THIS IS NEEDED. MuPDF writes `%PDF-<version/10>.<version%10>` from
// pdf_document::version, and it sets that field from the catalog /Version
// UNCONDITIONALLY at open. So a document authored with header 1.7 and
// `/Version /1.4` — legal, and per §7.5.5 still a 1.7 document because the
// override only applies upward — is re-emitted with a `%PDF-1.4` header. That
// is a silent downgrade of a saved file, reproducible against
// tests/fixtures/version_catalog_older_1_4.pdf.
//
// PREFIX SAFETY. This is called only from the FULL-REWRITE paths — save(),
// saveAs() and writeRecoveryCopy() — never from saveIncremental(), which
// appends to an existing file and must leave every prior byte untouched
// (see tests/test_incremental_update_integrity.cpp). Raising the writer's
// header field would be invisible to an append anyway, but correcting a stale
// catalog /Version below DOES touch a PDF object, so the call-site restriction
// is what guarantees prefix immutability, not the operation itself.
void Document::applyVersionFloorForSave() const {
    if (!pImpl->pdfDoc) return;
    const PdfVersion effective = pdfVersion();
    if (!effective.isValid()) return;

    if (effective.key() > pImpl->pdfDoc->version) {
        pImpl->pdfDoc->version = effective.key();
    }

    // Raising pdf_document::version alone is NOT sufficient, verified against
    // MuPDF 1.26.10: the emitted header still came out `%PDF-1.4` for the
    // 1.7-header / `/Version /1.4` fixture, because MuPDF re-derives the
    // version from the catalog on the way out. So the stale catalog value has
    // to be corrected too.
    //
    // This only ever fires when a catalog `/Version` ALREADY EXISTS and is
    // EARLIER than the document's effective version — i.e. exactly the input
    // that would otherwise be downgraded. It never adds the key to a document
    // that lacks it, and it can only raise the value.
    const auto cat = catalogVersion();
    if (!cat || !(*cat < effective)) return;

    const int key = effective.key();
    try {
        pImpl->context->execute([this, key](fz_context* ctx) {
            pdf_obj* root = pdf_dict_get(
                ctx, pdf_trailer(ctx, pImpl->pdfDoc), PDF_NAME(Root));
            if (!root) return;   // nothing to correct; the save itself will fail
            char name[8];
            name[0] = static_cast<char>('0' + (key / 10));
            name[1] = '.';
            name[2] = static_cast<char>('0' + (key % 10));
            name[3] = '\0';
            pdf_dict_put_name(ctx, root, PDF_NAME(Version), name);
        });
    } catch (const std::exception&) {
        // Correcting the version must never abort a save the user asked for.
        // Worst case the header keeps the stale value — the same behaviour as
        // before this fix, not a regression.
    }
}

int Document::authLevel() const noexcept {
    return pImpl->authLevel;
}

// Shared permission probe (R2-09/R2-10). Returns the requested MuPDF permission
// bit for THIS document. Unencrypted / non-PDF documents carry no restrictions,
// so every permission is granted. pdf_document_permissions reflects the access
// established by the password the document was opened with.
bool Document::permissionAllowed(int muPdfPermBit) const {
    if (!pImpl->pdfDoc) return true;
    return pImpl->context->executeReturn<bool>(
        [this, muPdfPermBit](fz_context* ctx) -> bool {
            pdf_obj* trailer = pdf_trailer(ctx, pImpl->pdfDoc);
            pdf_obj* enc = pdf_dict_get(ctx, trailer, PDF_NAME(Encrypt));
            if (!enc) return true;  // unencrypted: no restriction to enforce
            return (pdf_document_permissions(ctx, pImpl->pdfDoc) & muPdfPermBit) != 0;
        });
}

bool Document::printAllowed() const {
    return permissionAllowed(PDF_PERM_PRINT);
}

bool Document::copyAllowed() const {
    return permissionAllowed(PDF_PERM_COPY);
}

// PDF-23 (meta-audit follow-up F2): the remaining five of the eight standard
// ISO 32000-1 Table 22 permissions, plus high-resolution printing. Each shares
// the probe above so the "unencrypted grants everything" contract cannot drift
// between bits.
bool Document::modifyAllowed() const {
    return permissionAllowed(PDF_PERM_MODIFY);
}

bool Document::annotateAllowed() const {
    return permissionAllowed(PDF_PERM_ANNOTATE);
}

bool Document::fillFormsAllowed() const {
    return permissionAllowed(PDF_PERM_FORM);
}

bool Document::extractForAccessibilityAllowed() const {
    return permissionAllowed(PDF_PERM_ACCESSIBILITY);
}

bool Document::assembleAllowed() const {
    return permissionAllowed(PDF_PERM_ASSEMBLE);
}

bool Document::printHighResAllowed() const {
    return permissionAllowed(PDF_PERM_PRINT_HQ);
}

const std::filesystem::path& Document::filePath() const {
    return pImpl->filePath;
}

void Document::setFilePath(const std::filesystem::path& path) {
    pImpl->filePath = path;
}

void Document::registerScratchTempForCleanup(const std::filesystem::path& path) const {
    // #46: adopt ownership of a scratch/bake temp file this Document was reopened
    // ONTO (and now holds open) so it is deleted when we release our handle — the
    // dtor, or a later reopen onto a different file. If a prior scratch was
    // registered and is not the same file, delete it now (best-effort) so we do
    // not leak it by overwriting the record.
    if (!pImpl->scratchTempToDelete_.empty()
        && pImpl->scratchTempToDelete_ != path) {
        std::error_code ec;
        std::filesystem::remove(pImpl->scratchTempToDelete_, ec);
    }
    pImpl->scratchTempToDelete_ = path;
}

std::shared_ptr<MuPdfContext> Document::context() const {
    return pImpl->context;
}

std::vector<std::shared_ptr<BookmarkNode>> Document::extractBookmarks() {
    BookmarkExtractor extractor(pImpl->context);
    return extractor.extract(this);
}

fz_document* Document::fzDocument() const {
    return pImpl->doc;
}

bool Document::sessionRequiresFullRewrite() const {
    return pImpl->requiresFullRewrite_;
}

void Document::markRequiresFullRewrite() const {
    // pImpl is a unique_ptr member, so operator-> yields a mutable Impl*
    // even from this const method — flipping the bookkeeping bit is allowed.
    pImpl->requiresFullRewrite_ = true;
    // A destructive/structural mutation is a content edit — bump the version so
    // a direct mutator that only marks full-rewrite (redaction burn-in,
    // page-label rewrite, layer delete, …) is still visible to autosave.
    ++pImpl->contentVersion_;
}

bool Document::needsFullGc() const {
    return pImpl->needsFullGc_;
}

void Document::markNeedsFullGc() const {
    // A dedup-worthy op is also destructive/structural, so it must take the
    // full-rewrite path too — set both bits.
    pImpl->needsFullGc_ = true;
    pImpl->requiresFullRewrite_ = true;
    // …and it is a content edit — keep autosave's content-version signal in
    // step with markRequiresFullRewrite() (redaction apply routes here).
    ++pImpl->contentVersion_;
}

int Document::effectiveGarbageLevel() const {
    return pImpl->needsFullGc_ ? 3 : kDefaultFullRewriteGc;
}

bool Document::sourceUsesObjectStreams() const {
    return pImpl->sourceUsesObjectStreams_;
}

int Document::effectiveObjectStreamMode() const {
    return pImpl->sourceUsesObjectStreams_ ? 1 : 0;
}

// --- Page Manipulation ---

// R3-03 (PDF/UA structure-tree integrity across page ops).
//
// pdf_rearrange_pages(PDF_CLEAN_STRUCTURE_KEEP) rebuilds /StructTreeRoot's
// /ParentTree so it lists ONLY the retained pages' /StructParents entries, and
// leaves each surviving page's /StructParents value untouched (so those still
// resolve). But it does NOT prune the /StructTreeRoot /K tree: a /StructElem
// (or MCR/OBJR) whose /Pg points to a REMOVED page stays in the tree with a
// now-dangling /Pg — an orphan structure element that references a page no
// longer in the document. A PDF/UA validator (veraPDF) flags this, and it
// corrupts the logical reading order (a heading/paragraph pointing at nothing).
//
// Reconcile after the native rearrange: walk the /K graph and drop any subtree
// whose element's /Pg no longer resolves to a live page. Elements that carry no
// /Pg of their own (grouping elements such as /Document, /Sect) are kept and
// recursed into. The pruned objects become unreferenced and are collected by
// the mandatory full-rewrite/GC save that follows a page-tree change.
static bool pruneOrphanStructSubtree(fz_context* ctx, pdf_obj* elem,
                                     const std::set<int>& livePageNums,
                                     int depth) {
    if (depth > 128 || !elem || !pdf_is_dict(ctx, elem)) {
        return false;  // non-dict /K leaves (MCID ints) are never orphaned here
    }

    // If this element is anchored to a page (/Pg), and that page is gone, the
    // whole subtree is orphaned — signal the caller to remove it.
    pdf_obj* pg = pdf_dict_gets(ctx, elem, "Pg");
    if (pg) {
        int pgNum = pdf_to_num(ctx, pg);
        if (livePageNums.find(pgNum) == livePageNums.end()) {
            return true;
        }
    }

    // Recurse into /K, pruning orphaned children in place.
    pdf_obj* k = pdf_dict_get(ctx, elem, PDF_NAME(K));
    if (!k) return false;

    if (pdf_is_array(ctx, k)) {
        int n = pdf_array_len(ctx, k);
        for (int j = n - 1; j >= 0; --j) {
            pdf_obj* child = pdf_array_get(ctx, k, j);
            if (pdf_is_dict(ctx, child) &&
                pruneOrphanStructSubtree(ctx, child, livePageNums, depth + 1)) {
                pdf_array_delete(ctx, k, j);
            }
        }
    } else if (pdf_is_dict(ctx, k)) {
        if (pruneOrphanStructSubtree(ctx, k, livePageNums, depth + 1)) {
            pdf_dict_del(ctx, elem, PDF_NAME(K));
        }
    }
    return false;
}

// GitHub #196 — `livePageNums` is NOT a local here, and that is the fix.
//
// This function runs one call deep inside the fz_try that lives in
// MuPdfContext::execute (see Document::rearrangePages). fz_try/fz_catch is
// setjmp/longjmp: a fz_throw from pdf_count_pages, pdf_lookup_page_obj or any
// pdf_dict_* below longjmps straight to execute's fz_catch, abandoning every
// frame in between WITHOUT running C++ destructors. A `std::set<int>` declared
// here therefore leaked one heap node per live page on that path.
//
// The fix is ownership, not cleanup: the set is constructed in
// Document::rearrangePages' frame, which is ABOVE the setjmp, and passed in by
// reference. execute() turns the longjmp into a C++ MuPdfException *after* its
// fz_catch, so the owning frame unwinds normally and ~set() always runs -- on
// the success path, on the throw path, and on any future throw path added
// later. A hand-written `livePageNums.clear()` before each fz_throw would have
// been the manual-cleanup shape this project has been bitten by: correct only
// for the throw sites that existed when it was written.
//
// `scratch` is required to be empty on entry and is left populated on exit;
// the owner's destructor is the only thing that frees it.
static void reconcileStructTreeAfterRearrange(fz_context* ctx, pdf_document* doc,
                                              std::set<int>& livePageNums) {
    pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
    pdf_obj* str = root ? pdf_dict_get(ctx, root, PDF_NAME(StructTreeRoot)) : nullptr;
    if (!str) return;
    pdf_obj* rootK = pdf_dict_get(ctx, str, PDF_NAME(K));
    if (!rootK) return;

    // Live page object numbers after the rearrange. Owned by the caller's
    // frame (above the setjmp) -- see the note above.
    int pageCount = pdf_count_pages(ctx, doc);
    for (int i = 0; i < pageCount; ++i) {
        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, doc, i);
        if (pageObj) livePageNums.insert(pdf_to_num(ctx, pageObj));
    }

    if (pdf_is_array(ctx, rootK)) {
        int n = pdf_array_len(ctx, rootK);
        for (int j = n - 1; j >= 0; --j) {
            pdf_obj* child = pdf_array_get(ctx, rootK, j);
            if (pdf_is_dict(ctx, child) &&
                pruneOrphanStructSubtree(ctx, child, livePageNums, 0)) {
                pdf_array_delete(ctx, rootK, j);
            }
        }
    } else if (pdf_is_dict(ctx, rootK)) {
        if (pruneOrphanStructSubtree(ctx, rootK, livePageNums, 0)) {
            pdf_dict_del(ctx, str, PDF_NAME(K));
        }
    }
}

// F6 / PDF-90: pdf_rearrange_pages rebuilds the catalog without /AcroForm, and
// Document::rearrangePages restores it wholesale from kPreservedKeys[]. That
// restore is necessary -- without it every form is lost on any page edit -- but
// on its own it also resurrects the fields whose widgets lived on the pages that
// were just removed, VALUES INCLUDED. Deleting a page therefore left that page's
// entered form data reachable in the saved file. Prune the field tree down to
// the widgets that are still on a live page.
static bool pruneOrphanFormField(fz_context* ctx, pdf_obj* fieldRef,
                                 const std::set<int>& liveAnnots, int depth) {
    if (depth > 32) return false;
    pdf_obj* field = pdf_resolve_indirect(ctx, fieldRef);
    if (!pdf_is_dict(ctx, field)) return true;

    pdf_obj* kids = pdf_resolve_indirect(ctx, pdf_dict_get(ctx, field, PDF_NAME(Kids)));
    if (pdf_is_array(ctx, kids)) {
        for (int j = pdf_array_len(ctx, kids) - 1; j >= 0; --j) {
            if (pruneOrphanFormField(ctx, pdf_array_get(ctx, kids, j), liveAnnots,
                                     depth + 1)) {
                pdf_array_delete(ctx, kids, j);
            }
        }
        if (pdf_array_len(ctx, kids) > 0) return false;   // still has live widgets
        pdf_dict_del(ctx, field, PDF_NAME(Kids));
    }

    // Leaf. Only judge nodes we can positively identify as page-anchored: a
    // widget, or anything carrying a /P page reference. A field with neither is
    // left alone rather than guessed at.
    const bool isWidget =
        pdf_name_eq(ctx, pdf_dict_get(ctx, field, PDF_NAME(Subtype)), PDF_NAME(Widget));
    pdf_obj* pageRef = pdf_dict_get(ctx, field, PDF_NAME(P));
    if (!isWidget && !pageRef) return false;
    return liveAnnots.count(pdf_to_num(ctx, fieldRef)) == 0;
}

// GitHub #196, sibling instance. Found by the sweep the issue asked for rather
// than by the issue itself, which names only reconcileStructTreeAfterRearrange:
// this function is called from the same guarded region, one line later, and had
// exactly the same `std::set<int>` local. `liveAnnots` is now owned by
// Document::rearrangePages' frame for the reason documented above.
static void pruneAcroFormAfterRearrange(fz_context* ctx, pdf_document* doc,
                                        std::set<int>& liveAnnots) {
    pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
    pdf_obj* acro = pdf_resolve_indirect(ctx, pdf_dict_get(ctx, root, PDF_NAME(AcroForm)));
    if (!pdf_is_dict(ctx, acro)) return;
    pdf_obj* fields = pdf_resolve_indirect(ctx, pdf_dict_get(ctx, acro, PDF_NAME(Fields)));
    if (!pdf_is_array(ctx, fields)) return;

    const int pageCount = pdf_count_pages(ctx, doc);
    for (int i = 0; i < pageCount; ++i) {
        pdf_obj* page = pdf_lookup_page_obj(ctx, doc, i);
        pdf_obj* annots = pdf_resolve_indirect(ctx, pdf_dict_get(ctx, page, PDF_NAME(Annots)));
        const int n = pdf_array_len(ctx, annots);
        for (int a = 0; a < n; ++a) {
            liveAnnots.insert(pdf_to_num(ctx, pdf_array_get(ctx, annots, a)));
        }
    }

    for (int j = pdf_array_len(ctx, fields) - 1; j >= 0; --j) {
        if (pruneOrphanFormField(ctx, pdf_array_get(ctx, fields, j), liveAnnots, 0)) {
            pdf_array_delete(ctx, fields, j);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// GitHub #197 — the O(pages) page-REMOVAL path.
//
// #183 fixed the reorder half of the super-linear page-edit cost by not
// provoking MuPDF's page-map thrash: permutePageTreeInPlace() below resolves
// every page object once while the map is warm and then does pure pointer work.
// Deletion could not use it. A keep-list shorter than the page count is not a
// permutation, so it fell through to pdf_rearrange_pages and kept paying the
// quadratic: retainpages() calls pdf_lookup_page_obj once per retained page
// while writing each page's /Parent, and prepare_object_for_alteration
// (source/pdf/pdf-object.c:1818) drops doc->fwd_page_map / rev_page_map on
// every structural mutation, so each of the N lookups rebuilds the whole
// N-entry map -- an O(N) tree walk, N pdf_keep_obj calls and an O(N log N)
// qsort, N times. MEASURED on this tree before this change, over 50->400 pages:
// delete cost grew as pages^1.82 (docs/audit/p87-open-issues/phase-2).
//
// WHY IT COULD NOT JUST REUSE THE PERMUTATION PATH, and what that costs here.
// #183's fast path is sound precisely because a full permutation REMOVES
// NOTHING, which makes every pruning pass inside pdf_rearrange_pages a no-op by
// construction. A removal needs all of them, so they are reimplemented below --
// deliberately as a transcription of MuPDF's own algorithms in
// source/pdf/pdf-clean-file.c rather than as a fresh design, so the behaviour
// the existing delete suites pin (dangling GoTo dests stripped, orphaned
// /StructElem dropped, widgets on removed pages gone, outline entries to
// nowhere spliced out) is the behaviour they keep getting:
//
//   pdfeDestIsLive          <- dest_is_valid       (pdf-clean-file.c:99)
//   pdfePruneDestNameTree   <- the olddests rebuild (pdf-clean-file.c:336)
//   pdfeStripOutlineLevel   <- strip_outline        (pdf-clean-file.c:161)
//   pdfeStripOutlines       <- strip_outlines       (pdf-clean-file.c:226)
//
// ONE DELIBERATE DIVERGENCE, stated rather than hidden: MuPDF's page-object
// validity test is a LINEAR SCAN of page_object_nums per destination
// (dest_is_valid_page, pdf-clean-file.c:84), which is itself O(dests x pages).
// These use a std::set lookup instead. Same verdict, O(log pages).
//
// A SECOND, LARGER DIVERGENCE, also deliberate: MuPDF rebuilds
// /StructTreeRoot/ParentTree from scratch containing ONLY the retained pages'
// /StructParents entries, which silently discards every annotation's
// /StructParent entry as well. This path PRUNES the existing tree instead --
// it removes the entries belonging to removed pages and keeps the rest. That
// retains strictly more valid structure than the general path does.
//
// EVERY std::* container these need is owned by Document::rearrangePages'
// frame and passed in by reference. That is GitHub #196's rule, and it applies
// with more force here than anywhere else in the file: this is the largest
// block of code in PDFE that runs one call deep inside MuPdfContext::execute's
// fz_try, and a longjmp from any pdf_* call in it runs no destructor.
// ═══════════════════════════════════════════════════════════════════════════

// All the scratch the removal path needs, owned ABOVE the setjmp.
struct PageRemovalScratch {
    std::vector<pdf_obj*> pageObjs;        // every page object, index-ordered
    std::set<int> livePageNums;            // object numbers of retained pages
    std::set<int> deadStructParents;       // /StructParents of removed pages
    std::set<std::string> liveDestNames;   // named dests that survived pruning
    std::set<int> outlineSeen;             // cycle guard for the outline walk
    std::set<int> liveAnnots;              // scratch for pruneAcroFormAfterRearrange
    std::vector<char> retained;            // index -> is this page kept?
};

// Mirrors dest_is_valid. An object carrying /A <</S /GoTo /D ...>> and/or
// /Dest is live iff every destination it names still resolves. An object with
// no destination at all is live (a /Link with a /URI, an outline heading).
static bool pdfeDestIsLive(fz_context* ctx, pdf_obj* o,
                           const std::set<int>& livePages,
                           const std::set<std::string>& liveNames) {
    pdf_obj* a = pdf_dict_get(ctx, o, PDF_NAME(A));
    if (pdf_name_eq(ctx, pdf_dict_get(ctx, a, PDF_NAME(S)), PDF_NAME(GoTo))) {
        pdf_obj* d = pdf_dict_get(ctx, a, PDF_NAME(D));
        if (pdf_is_array(ctx, d)) {
            if (livePages.count(pdf_to_num(ctx, pdf_array_get(ctx, d, 0))) == 0)
                return false;
        } else if (pdf_is_string(ctx, d)) {
            const char* s = pdf_to_text_string(ctx, d);
            if (!s || liveNames.count(s) == 0) return false;
        } else if (pdf_is_name(ctx, d)) {
            const char* s = pdf_to_name(ctx, d);
            if (!s || liveNames.count(s) == 0) return false;
        }
    }

    pdf_obj* p = pdf_dict_get(ctx, o, PDF_NAME(Dest));
    if (!p) return true;                 // "a name with no dest counts as valid"
    if (pdf_is_string(ctx, p)) {
        const char* s = pdf_to_text_string(ctx, p);
        return s && liveNames.count(s) != 0;
    }
    if (pdf_is_name(ctx, p)) {
        const char* s = pdf_to_name(ctx, p);
        return s && liveNames.count(s) != 0;
    }
    return livePages.count(pdf_to_num(ctx, pdf_array_get(ctx, p, 0))) != 0;
}

// Walk a /Names-style name tree, delete every entry whose destination page is
// gone, and collect the keys that survive. `liveNames` is what makes the
// string-dest branch of pdfeDestIsLive answerable, so this MUST run before any
// link or outline is judged.
static void pdfePruneDestNameTree(fz_context* ctx, pdf_document* doc,
                                  pdf_obj* node,
                                  const std::set<int>& livePages,
                                  std::set<std::string>& liveNames, int depth) {
    if (depth > 64 || !pdf_is_dict(ctx, node)) return;

    pdf_obj* kids = pdf_dict_get(ctx, node, PDF_NAME(Kids));
    if (pdf_is_array(ctx, kids)) {
        const int n = pdf_array_len(ctx, kids);
        for (int i = 0; i < n; ++i) {
            pdfePruneDestNameTree(ctx, doc, pdf_array_get(ctx, kids, i), livePages,
                                  liveNames, depth + 1);
        }
        return;
    }

    pdf_obj* names = pdf_dict_get(ctx, node, PDF_NAME(Names));
    if (!pdf_is_array(ctx, names)) return;

    // Backwards, in key/value pairs, so a delete cannot disturb the indices
    // still to be visited.
    for (int i = pdf_array_len(ctx, names) - 2; i >= 0; i -= 2) {
        pdf_obj* val = pdf_array_get(ctx, names, i + 1);
        pdf_obj* d = pdf_dict_get(ctx, val, PDF_NAME(D));
        pdf_obj* arr = d ? d : val;
        const bool live =
            livePages.count(pdf_to_num(ctx, pdf_array_get(ctx, arr, 0))) != 0;
        if (live) {
            // No pdf_* call may happen while the temporary string is alive --
            // read the C string first, insert second (GitHub #196's rule, at
            // the granularity of a single expression).
            const char* key = pdf_to_text_string(ctx, pdf_array_get(ctx, names, i));
            if (key) liveNames.insert(key);
        } else {
            pdf_array_delete(ctx, names, i + 1);
            pdf_array_delete(ctx, names, i);
        }
    }

    // /Limits must describe what the node now holds. A node whose limits are
    // wider than its contents still resolves, but it is not conformant and a
    // validator will say so.
    const int left = pdf_array_len(ctx, names);
    if (left == 0) {
        pdf_dict_del(ctx, node, PDF_NAME(Limits));
    } else if (pdf_dict_get(ctx, node, PDF_NAME(Limits))) {
        pdf_obj* limits = pdf_new_array(ctx, doc, 2);
        pdf_dict_put_drop(ctx, node, PDF_NAME(Limits), limits);
        pdf_array_push(ctx, limits, pdf_array_get(ctx, names, 0));
        pdf_array_push(ctx, limits, pdf_array_get(ctx, names, left - 2));
    }
}

static int pdfeStripOutlines(fz_context* ctx, pdf_obj* outlines,
                             const std::set<int>& livePages,
                             const std::set<std::string>& liveNames,
                             std::set<int>& seen, int depth);

// Mirrors strip_outline: walk one /Next chain, dropping entries whose
// destination is gone AND which have no surviving children, splicing the chain
// around them. An entry with dead dest but live children keeps its place and
// loses only its destination -- exactly MuPDF's rule, and the reason a deleted
// page cannot orphan a whole bookmark subtree.
static int pdfeStripOutlineLevel(fz_context* ctx, pdf_obj* firstItem,
                                 const std::set<int>& livePages,
                                 const std::set<std::string>& liveNames,
                                 std::set<int>& seen,
                                 pdf_obj** pfirst, pdf_obj** plast, int depth) {
    pdf_obj* prev = nullptr;
    pdf_obj* first = nullptr;
    pdf_obj* current = firstItem;
    int count = 0;

    for (int guard = 0; current != nullptr && guard < 1000000; ++guard) {
        const int nc = pdfeStripOutlines(ctx, current, livePages, liveNames,
                                         seen, depth + 1);
        if (!pdfeDestIsLive(ctx, current, livePages, liveNames)) {
            if (nc == 0) {
                pdf_obj* next = pdf_dict_get(ctx, current, PDF_NAME(Next));
                if (!pdf_is_dict(ctx, next)) {
                    if (prev) pdf_dict_del(ctx, prev, PDF_NAME(Next));
                } else if (prev) {
                    pdf_dict_put(ctx, prev, PDF_NAME(Next), next);
                    pdf_dict_put(ctx, next, PDF_NAME(Prev), prev);
                } else {
                    pdf_dict_del(ctx, next, PDF_NAME(Prev));
                }
                current = pdf_is_dict(ctx, next) ? next : nullptr;
            } else {
                pdf_dict_del(ctx, current, PDF_NAME(Dest));
                pdf_dict_del(ctx, current, PDF_NAME(A));
                pdf_obj* next = pdf_dict_get(ctx, current, PDF_NAME(Next));
                current = pdf_is_dict(ctx, next) ? next : nullptr;
            }
        } else {
            if (!first) first = current;
            prev = current;
            pdf_obj* next = pdf_dict_get(ctx, current, PDF_NAME(Next));
            current = pdf_is_dict(ctx, next) ? next : nullptr;
            ++count;
        }
    }

    *pfirst = first;
    *plast = prev;
    return count;
}

// Mirrors strip_outlines. Returns the number of surviving children of `node`.
static int pdfeStripOutlines(fz_context* ctx, pdf_obj* outlines,
                             const std::set<int>& livePages,
                             const std::set<std::string>& liveNames,
                             std::set<int>& seen, int depth) {
    if (depth > 512 || !pdf_is_dict(ctx, outlines)) return 0;

    // Cycle guard. Only indirect nodes can be revisited; a direct dict has
    // object number 0 and cannot be reached twice through /First or /Next.
    const int num = pdf_to_num(ctx, outlines);
    if (num != 0 && !seen.insert(num).second) {
        fz_throw(ctx, FZ_ERROR_FORMAT, "cycle detected in outlines");
    }

    pdf_obj* first = pdf_dict_get(ctx, outlines, PDF_NAME(First));
    pdf_obj* last = nullptr;
    int nc = 0;
    if (pdf_is_dict(ctx, first)) {
        nc = pdfeStripOutlineLevel(ctx, first, livePages, liveNames, seen,
                                   &first, &last, depth);
    }

    if (nc == 0) {
        pdf_dict_del(ctx, outlines, PDF_NAME(First));
        pdf_dict_del(ctx, outlines, PDF_NAME(Last));
        pdf_dict_del(ctx, outlines, PDF_NAME(Count));
    } else {
        const int oldCount = pdf_dict_get_int(ctx, outlines, PDF_NAME(Count));
        pdf_dict_put(ctx, outlines, PDF_NAME(First), first);
        pdf_dict_put(ctx, outlines, PDF_NAME(Last), last);
        pdf_dict_put_int(ctx, outlines, PDF_NAME(Count), oldCount > 0 ? nc : -nc);
    }
    return nc;
}

// Drop the /ParentTree number-tree entries that belong to removed pages.
// MuPDF rebuilds this array from nothing; pruning keeps the annotation
// /StructParent entries that the rebuild throws away (see the header note).
static void pdfePruneParentTree(fz_context* ctx, pdf_document* doc, pdf_obj* node,
                                const std::set<int>& deadKeys, int depth) {
    if (depth > 64 || !pdf_is_dict(ctx, node) || deadKeys.empty()) return;

    pdf_obj* kids = pdf_dict_get(ctx, node, PDF_NAME(Kids));
    if (pdf_is_array(ctx, kids)) {
        const int n = pdf_array_len(ctx, kids);
        for (int i = 0; i < n; ++i)
            pdfePruneParentTree(ctx, doc, pdf_array_get(ctx, kids, i), deadKeys, depth + 1);
        return;
    }

    pdf_obj* nums = pdf_dict_get(ctx, node, PDF_NAME(Nums));
    if (!pdf_is_array(ctx, nums)) return;
    for (int i = pdf_array_len(ctx, nums) - 2; i >= 0; i -= 2) {
        const int key = pdf_array_get_int(ctx, nums, i);
        if (deadKeys.count(key) != 0) {
            pdf_array_delete(ctx, nums, i + 1);
            pdf_array_delete(ctx, nums, i);
        }
    }

    const int left = pdf_array_len(ctx, nums);
    if (left == 0) {
        pdf_dict_del(ctx, node, PDF_NAME(Limits));
    } else if (pdf_dict_get(ctx, node, PDF_NAME(Limits))) {
        pdf_obj* limits = pdf_new_array(ctx, doc, 2);
        pdf_dict_put_drop(ctx, node, PDF_NAME(Limits), limits);
        pdf_array_push_int(ctx, limits, pdf_array_get_int(ctx, nums, 0));
        pdf_array_push_int(ctx, limits, pdf_array_get_int(ctx, nums, left - 2));
    }
}

// Remove /Link annotations whose destination is gone, and strip the /A action
// off a widget whose action's destination is gone. Mirrors the two annotation
// loops of pdf_rearrange_pages_imp. Runs over the RETAINED page objects we
// already hold, so it costs no page lookups at all.
static void pdfePruneAnnotDestinations(fz_context* ctx,
                                       const std::vector<pdf_obj*>& pageObjs,
                                       const std::vector<int>& keep,
                                       const std::set<int>& livePageNums,
                                       const std::set<std::string>& liveNames) {
    for (int idx : keep) {
        pdf_obj* page = pageObjs[static_cast<size_t>(idx)];
        pdf_obj* annots = pdf_resolve_indirect(ctx, pdf_dict_get(ctx, page, PDF_NAME(Annots)));
        if (!pdf_is_array(ctx, annots)) continue;
        for (int j = pdf_array_len(ctx, annots) - 1; j >= 0; --j) {
            pdf_obj* o = pdf_array_get(ctx, annots, j);
            pdf_obj* subtype = pdf_dict_get(ctx, o, PDF_NAME(Subtype));
            if (pdf_name_eq(ctx, subtype, PDF_NAME(Link))) {
                if (!pdfeDestIsLive(ctx, o, livePageNums, liveNames))
                    pdf_array_delete(ctx, annots, j);
            } else if (pdf_name_eq(ctx, subtype, PDF_NAME(Widget))) {
                // Terminal field with an action to nowhere: drop the action,
                // never the field -- its VALUE is user data.
                pdf_obj* f = o;
                for (int up = 0; up < 32; ++up) {
                    pdf_obj* parent = pdf_dict_get(ctx, f, PDF_NAME(Parent));
                    if (!pdf_is_dict(ctx, parent)) break;
                    f = parent;
                }
                if (!pdfeDestIsLive(ctx, f, livePageNums, liveNames))
                    pdf_dict_del(ctx, f, PDF_NAME(A));
            }
        }
    }
}

// The removal fast path itself. Returns false -- having mutated NOTHING --
// when the document is not eligible, in which case the caller must run the
// general pdf_rearrange_pages path.
static bool retainPageTreeInPlace(fz_context* ctx, pdf_document* doc,
                                  const std::vector<int>& keep,
                                  PageRemovalScratch& s) {
    const int total = pdf_count_pages(ctx, doc);
    const int kept = static_cast<int>(keep.size());
    if (kept <= 0 || kept >= total) return false;
    if (static_cast<int>(s.pageObjs.size()) != total) return false;
    if (static_cast<int>(s.retained.size()) != total) return false;

    // Strictly increasing, in range: a pure removal that preserves the order of
    // what is left. Anything else -- a duplicate, a reordering, a reordering
    // combined with a removal -- is NOT eligible and falls back. That is the
    // whole eligibility rule and it is deliberately narrow: every case this
    // path handles is one where the surviving pages' relative order, and
    // therefore every page-position-dependent structure other than the page
    // tree itself, is unchanged.
    for (int i = 0; i < kept; ++i) {
        if (keep[i] < 0 || keep[i] >= total) return false;
        if (i > 0 && keep[i] <= keep[i - 1]) return false;
    }

    pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
    pdf_obj* pages = pdf_dict_get(ctx, root, PDF_NAME(Pages));
    if (!pdf_is_dict(ctx, pages)) return false;
    // The PDF-1.1 catalog /Dests DICTIONARY is migrated into /Names/Dests by
    // pdf_rearrange_pages. Reproducing that migration here would be the
    // page-tree-semantics rewrite this path exists to avoid, so a document
    // carrying one is not eligible -- same rule as permutePageTreeInPlace.
    if (pdf_dict_get(ctx, root, PDF_NAME(Dests))) return false;

    // ---- PHASE 1: resolve only. Nothing is mutated, so a malformed page tree
    //      that throws here can still fall back to the general path.
    for (int i = 0; i < total; ++i) s.pageObjs[i] = nullptr;
    int resolved = 1;
    fz_var(resolved);
    fz_try(ctx) {
        for (int i = 0; i < total; ++i)
            s.pageObjs[i] = pdf_keep_obj(ctx, pdf_lookup_page_obj(ctx, doc, i));
    }
    fz_catch(ctx) {
        fz_ignore_error(ctx);
        resolved = 0;
    }
    if (!resolved) {
        for (int i = 0; i < total; ++i) {
            if (s.pageObjs[i]) { pdf_drop_obj(ctx, s.pageObjs[i]); s.pageObjs[i] = nullptr; }
        }
        return false;
    }

    // Which page objects survive, and which /StructParents keys die with the
    // pages that carried them. Both are read BEFORE anything is mutated.
    // `retained` lives in the caller-owned scratch, not on this stack: this
    // function runs inside MuPdfContext::execute's fz_try and the pdf_dict_get
    // calls in the loop below can longjmp (GitHub #196). It is SIZED by the
    // caller too, so nothing here allocates inside the guarded region.
    {
        for (auto& c : s.retained) c = 0;
        for (int idx : keep) s.retained[static_cast<size_t>(idx)] = 1;
        for (int i = 0; i < total; ++i) {
            if (s.retained[static_cast<size_t>(i)]) {
                s.livePageNums.insert(pdf_to_num(ctx, s.pageObjs[i]));
            } else if (pdf_dict_get(ctx, s.pageObjs[i], PDF_NAME(StructParents))) {
                s.deadStructParents.insert(
                    pdf_dict_get_int(ctx, s.pageObjs[i], PDF_NAME(StructParents)));
            }
        }
    }

    // ---- PHASE 2: mutate. From here a failure is a genuine failure.
    pdf_obj* kids = nullptr;
    fz_var(kids);
    fz_try(ctx) {
        kids = pdf_new_array(ctx, doc, kept);
        for (int idx : keep) {
            pdf_obj* page = s.pageObjs[idx];
            // Same ORDER as MuPDF's retainpage(): flatten before re-parenting,
            // so inherited /Resources, /MediaBox, /CropBox and /Rotate are
            // still read from the page's ORIGINAL parent chain.
            pdf_flatten_inheritable_page_items(ctx, page);
            pdf_dict_put(ctx, page, PDF_NAME(Parent), pages);
            pdf_array_push(ctx, kids, page);
        }
        pdf_dict_put_int(ctx, pages, PDF_NAME(Count), kept);
        pdf_dict_put(ctx, pages, PDF_NAME(Kids), kids);

        // PDF-95 parity with both other paths: /OpenAction and /Threads are
        // page-position dependent and PDFE has no remapping for them.
        pdf_dict_dels(ctx, root, "OpenAction");
        pdf_dict_dels(ctx, root, "Threads");

        // ---- PRUNE, in dependency order. Named destinations first: every
        //      later test of a string destination asks whether its name is
        //      still in the tree, so judging a link before the tree is pruned
        //      would keep a link to a destination that is about to go.
        pdf_obj* names = pdf_dict_get(ctx, root, PDF_NAME(Names));
        pdf_obj* dests = pdf_dict_get(ctx, names, PDF_NAME(Dests));
        if (dests) {
            pdfePruneDestNameTree(ctx, doc, dests, s.livePageNums, s.liveDestNames, 0);
        }

        pdfePruneAnnotDestinations(ctx, s.pageObjs, keep, s.livePageNums,
                                   s.liveDestNames);

        pdf_obj* outlines = pdf_dict_get(ctx, root, PDF_NAME(Outlines));
        if (pdf_is_dict(ctx, outlines)) {
            if (pdfeStripOutlines(ctx, outlines, s.livePageNums, s.liveDestNames,
                                  s.outlineSeen, 0) == 0) {
                pdf_dict_del(ctx, root, PDF_NAME(Outlines));
            }
        }

        pdf_obj* str = pdf_dict_get(ctx, root, PDF_NAME(StructTreeRoot));
        pdf_obj* parentTree = pdf_dict_get(ctx, str, PDF_NAME(ParentTree));
        if (parentTree)
            pdfePruneParentTree(ctx, doc, parentTree, s.deadStructParents, 0);

        // R3-03 and F6 / PDF-90, the two passes the general path already runs.
        reconcileStructTreeAfterRearrange(ctx, doc, s.livePageNums);
        pruneAcroFormAfterRearrange(ctx, doc, s.liveAnnots);
    }
    fz_always(ctx) {
        pdf_drop_obj(ctx, kids);
        for (int i = 0; i < total; ++i) {
            if (s.pageObjs[i]) { pdf_drop_obj(ctx, s.pageObjs[i]); s.pageObjs[i] = nullptr; }
        }
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
    return true;
}

// GitHub #183 — the O(N) path for the one rearrangement that removes nothing.
//
// WHY THE GENERAL PATH IS QUADRATIC, measured rather than assumed. MuPDF
// invalidates its page map (doc->fwd_page_map / rev_page_map) inside
// prepare_object_for_alteration() on EVERY structural object mutation
// (pdf-object.c:1818). pdf_rearrange_pages' retainpage() mutates each page's
// /Parent (pdf-clean-file.c:52), so the map is dropped once per page — and the
// next iteration's pdf_lookup_page_obj() rebuilds the whole N-entry map from
// scratch (pdf-page.c:113: an O(N) tree walk, N pdf_keep_obj calls and an
// O(N log N) qsort). N rebuilds of an N-entry map is the pages^1.86 curve
// measured across 100/200/400/800 pages in the phase-5 benchmark artifact. It
// is entirely inside MuPDF: prepare_object_for_alteration runs before
// pdf_dict_get_put even compares the old value, so pre-setting /Parent to the
// value MuPDF is about to write does not avoid it, and doc->non_structural_change
// (the flag that would suppress it) cannot be used here because the /Kids swap
// later in the same call IS structural — suppressing the drop would leave a
// stale map that then answers with the OLD page order.
//
// WHAT THIS DOES INSTEAD. Resolve every page object ONCE, up front, while the
// map is still warm — reads never invalidate it. Everything after that is
// pointer work on objects already in hand: flatten inherited attributes,
// re-parent, push into a new /Kids array. One O(N) pass, zero lookups after the
// first mutation.
//
// WHAT IT DELIBERATELY DOES NOT DO, and why that is correctness rather than a
// shortcut. Every pruning step inside pdf_rearrange_pages is defined by the
// pages that were REMOVED: dangling GoTo destinations, /StructElem whose /Pg is
// gone, widgets whose page is gone, outline entries pointing nowhere, links to
// nowhere. A full permutation removes nothing, so every one of them is a no-op
// by construction. PDF destinations are page-OBJECT references, not ordinals,
// so they keep resolving across a permutation without being rewritten — which
// is also why PDFE's own reconcileStructTreeAfterRearrange() and
// pruneAcroFormAfterRearrange() are skipped here: both are defined as "drop
// what points at a page that is no longer live", and every page is still live.
//
// PARITY WITH THE PDF-95 CONTRACT. /OpenAction and /Threads are page-position
// dependent and PDFE has no remapping for them, so the general path drops them
// and so must this one — test_page_op_preservation_matrix.cpp's
// RearrangeDropsThreadsAndOpenActionByDesign is that contract. The legacy
// PDF-1.1 catalog /Dests DICTIONARY is the one that is different:
// pdf_rearrange_pages does not drop it, it MIGRATES it into /Names/Dests
// (pdf_load_name_tree merges both spellings, and the rebuilt catalog re-emits
// them as a flat /Names list). Reproducing that migration here would be exactly
// the page-tree-semantics rewrite this fast path exists to avoid, so a document
// carrying one is NOT eligible and falls back to the general path unchanged.
//
// Returns false — having mutated nothing — if the document is not eligible, in
// which case the caller must run the general path.
static bool permutePageTreeInPlace(fz_context* ctx, pdf_document* doc,
                                   const std::vector<int>& order,
                                   std::vector<pdf_obj*>& pageObjs) {
    const int n = static_cast<int>(order.size());
    if (n <= 0 || static_cast<int>(pageObjs.size()) != n) return false;

    pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
    pdf_obj* pages = pdf_dict_get(ctx, root, PDF_NAME(Pages));
    if (!pdf_is_dict(ctx, pages)) return false;
    if (pdf_dict_get(ctx, root, PDF_NAME(Dests))) return false;  // legacy /Dests
    if (pdf_count_pages(ctx, doc) != n) return false;

    for (int i = 0; i < n; ++i) pageObjs[i] = nullptr;

    // PHASE 1 — resolve only. Nothing is mutated yet, so a malformed page tree
    // that throws here can still fall back to the general path, which has its
    // own (slower, more forgiving) traversal. `resolved` is fz_var'd because a
    // longjmp leaves a non-volatile local written after the setjmp indeterminate.
    int resolved = 1;
    fz_var(resolved);
    fz_try(ctx) {
        for (int i = 0; i < n; ++i) {
            pageObjs[i] = pdf_keep_obj(ctx, pdf_lookup_page_obj(ctx, doc, i));
        }
    }
    fz_catch(ctx) {
        fz_ignore_error(ctx);
        resolved = 0;
    }
    if (!resolved) {
        for (int i = 0; i < n; ++i) {
            if (pageObjs[i]) { pdf_drop_obj(ctx, pageObjs[i]); pageObjs[i] = nullptr; }
        }
        return false;
    }

    // PHASE 2 — mutate. From here a failure is a genuine failure and rethrows.
    pdf_obj* kids = nullptr;
    fz_var(kids);
    fz_try(ctx) {
        kids = pdf_new_array(ctx, doc, n);
        for (int k = 0; k < n; ++k) {
            pdf_obj* page = pageObjs[order[k]];
            // Same ORDER as MuPDF's retainpage(): flatten before re-parenting,
            // so inherited /Resources, /MediaBox, /CropBox and /Rotate are still
            // read from the page's ORIGINAL parent chain. Once /Kids below is
            // replaced, any intermediate /Pages node falls out of the document
            // and is swept by the full-rewrite save this method forces.
            pdf_flatten_inheritable_page_items(ctx, page);
            pdf_dict_put(ctx, page, PDF_NAME(Parent), pages);
            pdf_array_push(ctx, kids, page);
        }
        pdf_dict_put_int(ctx, pages, PDF_NAME(Count), n);
        pdf_dict_put(ctx, pages, PDF_NAME(Kids), kids);

        // PDF-95 parity — see the header note.
        pdf_dict_dels(ctx, root, "OpenAction");
        pdf_dict_dels(ctx, root, "Threads");
    }
    fz_always(ctx) {
        pdf_drop_obj(ctx, kids);
        for (int i = 0; i < n; ++i) {
            if (pageObjs[i]) { pdf_drop_obj(ctx, pageObjs[i]); pageObjs[i] = nullptr; }
        }
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
    return true;
}

void Document::rearrangePages(const std::vector<int>& keptOriginalIndicesInNewOrder) {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document");
    }
    const int oldCount = pImpl->cachedPageCount;
    const std::vector<int>& list = keptOriginalIndicesInNewOrder;
    if (list.empty()) {
        throw PdfeException("Cannot rearrange to zero pages");
    }
    for (int idx : list) {
        if (idx < 0 || idx >= oldCount) {
            throw PdfeException("Rearrange index out of range: " + std::to_string(idx));
        }
    }

    // Built HERE, in the caller's frame -- NOT inside the lambda. The lambda
    // body below is itself the body of MuPdfContext::execute's fz_try, so a
    // longjmp from any pdf_* call abandons the lambda's whole frame without
    // running destructors. Moving this to the top of the lambda would not
    // help: only a frame that exists BEFORE the setjmp is safe.
    std::vector<int> pageList(list.begin(), list.end());

    // GitHub #183. A FULL PERMUTATION -- same page set, new order -- is the one
    // rearrangement that removes nothing, and it is the one the general path
    // handles quadratically. Detect it here (in the caller's frame, before any
    // setjmp) and take permutePageTreeInPlace(), which is O(pages). Anything
    // that drops or duplicates a page falls through to the general path.
    bool isFullPermutation = list.size() == static_cast<size_t>(oldCount);
    if (isFullPermutation) {
        std::vector<char> seen(static_cast<size_t>(oldCount), 0);
        for (int idx : list) {
            if (seen[static_cast<size_t>(idx)]) { isFullPermutation = false; break; }
            seen[static_cast<size_t>(idx)] = 1;
        }
    }
    // Caller-frame scratch for the fast path, for the same reason pageList is
    // built here: it must outlive the setjmp, and it must not be constructed
    // inside the guarded region.
    std::vector<pdf_obj*> pageObjs;
    if (isFullPermutation) pageObjs.assign(static_cast<size_t>(oldCount), nullptr);

    bool permuted = false;
    if (isFullPermutation) {
        pImpl->context->execute([this, &pageList, &pageObjs, &permuted](fz_context* ctx) {
            permuted = permutePageTreeInPlace(ctx, pImpl->pdfDoc, pageList, pageObjs);
        });
    }

    if (permuted) {
        pImpl->cachedPageCount = static_cast<int>(list.size());
        pImpl->dirtyPageIndices_.clear();  // page tree rebuilt → stale dirty marks gone
        pImpl->markContentChanged();
        // NOT markNeedsFullGc(): do_garbage=3 exists to run MuPDF's
        // removeduplicateobjs (pdf-write.c:237), an all-pairs O(objects^2) scan
        // that is the other half of #183's super-linear curve (measured
        // pages^1.62 for the save). A permutation creates no object and
        // duplicates none -- it re-orders one /Kids array -- so there is nothing
        // for the dedup pass to find. The full rewrite is still mandatory (the
        // page tree changed), and its gc=1 sweep still collects the intermediate
        // /Pages nodes that the flattening orphaned. The general path below
        // keeps gc=3 untouched.
        markRequiresFullRewrite();
        return;
    }

    // GitHub #197. A PURE REMOVAL -- a strictly increasing subset of the pages,
    // relative order preserved -- is the other rearrangement shape the general
    // path handles quadratically, and it is the one deletePage() produces.
    // Detected here, in the caller's frame, before any setjmp, for the same
    // reason the permutation check is. Everything else (a removal that also
    // reorders, anything with a duplicate) still falls through.
    bool isPureRemoval = list.size() < static_cast<size_t>(oldCount) && !list.empty();
    if (isPureRemoval) {
        for (size_t i = 1; i < list.size(); ++i) {
            if (list[i] <= list[i - 1]) { isPureRemoval = false; break; }
        }
    }

    // Caller-frame scratch for the removal path, sized here so that nothing
    // inside the guarded region has to allocate a container (GitHub #196).
    PageRemovalScratch removalScratch;
    if (isPureRemoval) {
        removalScratch.pageObjs.assign(static_cast<size_t>(oldCount), nullptr);
        removalScratch.retained.assign(static_cast<size_t>(oldCount), 0);
    }

    bool removed = false;
    if (isPureRemoval) {
        pImpl->context->execute(
            [this, &pageList, &removalScratch, &removed](fz_context* ctx) {
                removed = retainPageTreeInPlace(ctx, pImpl->pdfDoc, pageList,
                                                removalScratch);
            });
    }

    if (removed) {
        pImpl->cachedPageCount = static_cast<int>(list.size());
        pImpl->dirtyPageIndices_.clear();  // page tree rebuilt → stale dirty marks gone
        pImpl->markContentChanged();
        // gc=3 IS kept here, unlike the permutation path. A removal orphans
        // real objects -- the removed page, its content streams, the /StructElem
        // subtrees and widgets pruned above -- and ConditionalGcTest.
        // PageDeleteNeedsFullGc pins that a page delete escalates to level 3.
        // That contract is not renegotiated under cover of a performance fix;
        // the measured save exponent (pages^1.31) is not the half of #197 that
        // is super-linear in the first place.
        markNeedsFullGc();
        return;
    }

    // GitHub #196. Scratch for the two post-rearrange pruning passes, built in
    // the CALLER's frame for the same reason pageList is: everything below runs
    // inside MuPdfContext::execute's fz_try, and a longjmp out of it does not
    // run destructors for frames it skips. Owned here, these two sets are freed
    // by ordinary C++ unwinding whether the rearrange succeeds or throws --
    // execute() rethrows a fz_throw as a C++ MuPdfException once it is back
    // outside the guarded region. Passing them in by reference is the RAII fix;
    // clearing them by hand at each throw site would not be.
    std::set<int> livePageNums;
    std::set<int> liveAnnots;

    pImpl->context->execute([this, &pageList, &livePageNums, &liveAnnots](fz_context* ctx) {
        pdf_document* doc = pImpl->pdfDoc;

        // pdf_rearrange_pages rebuilds the document /Root (catalog) keeping only
        // a handful of keys (Type, Pages, StructTreeRoot, Outlines, OCProperties,
        // Names/Dests). Every other catalog entry — /AcroForm, /Metadata, /Lang,
        // /MarkInfo, /ViewerPreferences, /PageLabels, ... — would otherwise be
        // dropped, silently losing forms and document metadata on any page edit.
        // Snapshot the page-position-INDEPENDENT ones (keep-ref so a GC inside
        // the rearrange can't free them) and re-attach after. Page-position-
        // DEPENDENT catalog entries that could dangle to a removed page
        // (/OpenAction, the legacy catalog /Dests, /Threads) are deliberately
        // NOT carried over — pdf_rearrange_pages governs named dests via the
        // rebuilt /Names tree.
        static const char* const kPreservedKeys[] = {
            "AcroForm", "Metadata", "Lang", "MarkInfo", "ViewerPreferences",
            "PageLabels", "PageLayout", "PageMode", "Perms", "Version",
            "Extensions", "Collection", "AF",
        };
        constexpr size_t nKeys = sizeof(kPreservedKeys) / sizeof(kPreservedKeys[0]);

        pdf_obj* oldRoot = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        pdf_obj* preserved[nKeys] = {nullptr};
        for (size_t k = 0; k < nKeys; ++k) {
            pdf_obj* v = pdf_dict_gets(ctx, oldRoot, kPreservedKeys[k]);
            preserved[k] = v ? pdf_keep_obj(ctx, v) : nullptr;
        }
        // F6 / PDF-93 + PDF-94: pdf_rearrange_pages rebuilds the catalog /Names
        // dictionary carrying ONLY /Dests, so /EmbeddedFiles (attachments),
        // /JavaScript (document JS) and every other page-INDEPENDENT name tree
        // were silently dropped by any delete or reorder. /Dests is excluded
        // from the restore on purpose: MuPDF rebuilds it, pruned to the pages
        // that survived, and re-attaching the old one would resurrect
        // destinations pointing at pages that no longer exist.
        pdf_obj* oldNames = pdf_dict_get(ctx, oldRoot, PDF_NAME(Names));
        pdf_obj* preservedNames = oldNames ? pdf_keep_obj(ctx, oldNames) : nullptr;

        fz_try(ctx) {
            pdf_rearrange_pages(ctx, doc, static_cast<int>(pageList.size()),
                                pageList.data(), PDF_CLEAN_STRUCTURE_KEEP);

            // The catalog object was replaced in place (same object number), so
            // re-fetch it and restore any preserved key the rebuilt root lacks.
            pdf_obj* newRoot = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
            for (size_t k = 0; k < nKeys; ++k) {
                if (preserved[k] &&
                    !pdf_dict_gets(ctx, newRoot, kPreservedKeys[k])) {
                    pdf_dict_puts(ctx, newRoot, kPreservedKeys[k], preserved[k]);
                }
            }

            if (preservedNames) {
                pdf_obj* newNames = pdf_dict_get(ctx, newRoot, PDF_NAME(Names));
                if (!newNames) {
                    pdf_obj* fresh = pdf_new_dict(ctx, doc, 4);
                    pdf_dict_put_drop(ctx, newRoot, PDF_NAME(Names),
                                      pdf_add_object_drop(ctx, doc, fresh));
                    newNames = pdf_dict_get(ctx, newRoot, PDF_NAME(Names));
                }
                const int nn = pdf_dict_len(ctx, preservedNames);
                for (int i = 0; i < nn; ++i) {
                    pdf_obj* key = pdf_dict_get_key(ctx, preservedNames, i);
                    if (pdf_name_eq(ctx, key, PDF_NAME(Dests))) continue;
                    if (!pdf_dict_get(ctx, newNames, key)) {
                        pdf_dict_put(ctx, newNames, key,
                                     pdf_dict_get_val(ctx, preservedNames, i));
                    }
                }
            }

            // R3-03: prune /StructElem subtrees orphaned to removed pages that
            // pdf_rearrange_pages leaves dangling in /StructTreeRoot's /K graph.
            reconcileStructTreeAfterRearrange(ctx, doc, livePageNums);

            // F6 / PDF-90: drop form fields whose widgets were on removed pages
            // so a deleted page cannot leave its entered VALUES behind.
            pruneAcroFormAfterRearrange(ctx, doc, liveAnnots);

            // No explicit page-tree drop: MuPDF invalidates its own page map
            // inside the object-mutation path (pdf-object.c:1818), and the
            // public pdf_drop_page_tree() is a no-op in 1.26.10. See
            // docs/audit/page-tree-flush-noop-2026-08-31.md.
        }
        fz_always(ctx) {
            for (size_t k = 0; k < nKeys; ++k) {
                if (preserved[k]) pdf_drop_obj(ctx, preserved[k]);
            }
            if (preservedNames) pdf_drop_obj(ctx, preservedNames);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });

    pImpl->cachedPageCount = static_cast<int>(list.size());
    pImpl->dirtyPageIndices_.clear();  // page tree rebuilt → stale dirty marks gone
    pImpl->markContentChanged();
    markNeedsFullGc();  // structural page-tree change → full rewrite + dedup
}

void Document::reorderPages(const std::vector<int>& newOrder) {
    if (static_cast<int>(newOrder.size()) != pImpl->cachedPageCount) {
        throw PdfeException("reorderPages requires a full permutation of all pages");
    }
    // Validate it is a genuine permutation of 0..count-1.
    std::vector<char> seen(pImpl->cachedPageCount, 0);
    for (int idx : newOrder) {
        if (idx < 0 || idx >= pImpl->cachedPageCount || seen[idx]) {
            throw PdfeException("reorderPages: not a valid permutation");
        }
        seen[idx] = 1;
    }
    rearrangePages(newOrder);
}

void Document::deletePage(int index) {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document");
    }
    if (index < 0 || index >= pImpl->cachedPageCount) {
        throw PdfeException("Page index out of range: " + std::to_string(index));
    }
    if (pImpl->cachedPageCount <= 1) {
        throw PdfeException("Cannot delete the last page");
    }

    // R3-01: route through pdf_rearrange_pages (via rearrangePages) so bookmark,
    // link and named-destination targets are remapped/stripped as the page tree
    // is rebuilt — a bare pdf_delete_page leaves every dest dangling or shifted.
    std::vector<int> keep;
    keep.reserve(pImpl->cachedPageCount - 1);
    for (int i = 0; i < pImpl->cachedPageCount; ++i) {
        if (i != index) keep.push_back(i);
    }
    rearrangePages(keep);
}

void Document::insertBlankPage(int atIndex, float widthPt, float heightPt, int rotation) {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document");
    }
    if (atIndex < 0 || atIndex > pImpl->cachedPageCount) {
        throw PdfeException("Insert index out of range: " + std::to_string(atIndex));
    }

    pImpl->context->execute([this, atIndex, widthPt, heightPt, rotation](fz_context* ctx) {
        fz_rect mediabox = fz_make_rect(0, 0, widthPt, heightPt);
        pdf_obj* pageObj = pdf_add_page(ctx, pImpl->pdfDoc, mediabox, rotation,
                                         nullptr, nullptr);
        pdf_insert_page(ctx, pImpl->pdfDoc, atIndex, pageObj);
        pdf_drop_obj(ctx, pageObj);
    });
    pImpl->cachedPageCount++;
    pImpl->markContentChanged();
    markRequiresFullRewrite();  // page tree change → full rewrite
}

namespace {
// A page retained on the undo stack for a page-structure command
// (Delete/Move/DeleteRange/Replace). Rather than a raw pdf_keep_obj into the
// LIVE document — which Document::save() drops+reopens and Document::saveAs()
// GC-renumbers, dangling the reference across a full-rewrite save (post-save
// undo = UAF / "container and item belong to different documents") — the page
// is deep-copied (pdf_graft_page) into its OWN standalone pdf_document. That
// holder lives in the shared fz_context, independent of the live document's
// lifetime, so it survives any save and can be re-grafted into whatever
// document is current at undo time. See PageCommands.cpp and
// tests/test_undo_after_save_crash.cpp.
struct RetainedPageHolder {
    pdf_document* doc = nullptr;  // standalone; the retained page is at index 0
    // Keep the shared MuPdfContext alive for the holder's whole lifetime so the
    // holder can be dropped WITHOUT a live Document — the owning Document may be
    // destroyed before the undo command that holds this handle (defect B2).
    std::shared_ptr<MuPdfContext> context;
};
} // namespace

void Document::insertPageObj(int atIndex, void* pageObj) {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document");
    }
    if (atIndex < 0 || atIndex > pImpl->cachedPageCount) {
        throw PdfeException("Insert index out of range: " + std::to_string(atIndex));
    }
    if (!pageObj) {
        throw PdfeException("Page object is null");
    }

    auto* holder = static_cast<RetainedPageHolder*>(pageObj);
    if (!holder->doc) {
        throw PdfeException("Retained page holder is empty");
    }
    pImpl->context->execute([this, atIndex, holder](fz_context* ctx) {
        // Graft a COPY of the retained page (held in its own standalone
        // document) into the live document. Non-consuming: the holder stays
        // valid so a later redo/undo cycle can re-graft it; releasePageObj()
        // frees it. Grafting a document-independent copy — rather than
        // pdf_insert_page of a kept obj into the live doc — is what lets undo
        // restore the page even after save() has dropped+reopened pImpl->pdfDoc.
        pdf_graft_page(ctx, pImpl->pdfDoc, atIndex, holder->doc, 0);
    });
    pImpl->cachedPageCount++;
    pImpl->markContentChanged();
    // Used by page move/reorder (re-inserting a retained page object); reorder
    // is a dedup-worthy structural change → full rewrite + dedup.
    markNeedsFullGc();
}

void Document::rotatePage(int index, int angleDelta) {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document");
    }
    if (index < 0 || index >= pImpl->cachedPageCount) {
        throw PdfeException("Page index out of range: " + std::to_string(index));
    }

    pImpl->context->execute([this, index, angleDelta](fz_context* ctx) {
        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pImpl->pdfDoc, index);
        int oldRotation = 0;
        pdf_obj* rotateObj = pdf_dict_get(ctx, pageObj, PDF_NAME(Rotate));
        if (rotateObj) {
            oldRotation = pdf_to_int(ctx, rotateObj);
        }
        int newRotation = ((oldRotation + angleDelta) % 360 + 360) % 360;
        pdf_dict_put_int(ctx, pageObj, PDF_NAME(Rotate), newRotation);
    });
    pImpl->markContentChanged();
    markRequiresFullRewrite();  // page-tree /Rotate change → full rewrite
}

int Document::currentRotation(int index) const {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document");
    }
    if (index < 0 || index >= pImpl->cachedPageCount) {
        throw PdfeException("Page index out of range: " + std::to_string(index));
    }

    return pImpl->context->executeReturn<int>(
        [this, index](fz_context* ctx) -> int {
            pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pImpl->pdfDoc, index);
            pdf_obj* rotateObj = pdf_dict_get(ctx, pageObj, PDF_NAME(Rotate));
            if (!rotateObj) return 0;
            return pdf_to_int(ctx, rotateObj);
        }
    );
}

void Document::graftPage(int atIndex, const Document& source, int sourcePageIndex) {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document");
    }
    if (!source.pImpl->pdfDoc) {
        throw PdfeException("Source is not a PDF document");
    }
    if (atIndex < 0 || atIndex > pImpl->cachedPageCount) {
        throw PdfeException("Insert index out of range: " + std::to_string(atIndex));
    }
    if (sourcePageIndex < 0 || sourcePageIndex >= source.pImpl->cachedPageCount) {
        throw PdfeException("Source page index out of range: " + std::to_string(sourcePageIndex));
    }

    auto* srcDoc = source.pImpl->pdfDoc;
    pImpl->context->execute([this, atIndex, srcDoc, sourcePageIndex](fz_context* ctx) {
        pdf_graft_page(ctx, pImpl->pdfDoc, atIndex, srcDoc, sourcePageIndex);
    });
    pImpl->cachedPageCount++;
    pImpl->markContentChanged();
    markNeedsFullGc();  // page import (graft) can duplicate resources → dedup
}

void Document::graftPages(int atIndex, const Document& source, int sourceStart, int sourceEnd) {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document");
    }
    if (!source.pImpl->pdfDoc) {
        throw PdfeException("Source is not a PDF document");
    }
    if (atIndex < 0 || atIndex > pImpl->cachedPageCount) {
        throw PdfeException("Insert index out of range: " + std::to_string(atIndex));
    }
    if (sourceStart < 0 || sourceEnd >= source.pImpl->cachedPageCount || sourceStart > sourceEnd) {
        throw PdfeException("Source page range out of range");
    }

    auto* srcDoc = source.pImpl->pdfDoc;
    int count = sourceEnd - sourceStart + 1;

    pImpl->context->execute([this, atIndex, srcDoc, sourceStart, count](fz_context* ctx) {
        pdf_graft_map* graftMap = pdf_new_graft_map(ctx, pImpl->pdfDoc);
        for (int i = 0; i < count; i++) {
            pdf_graft_mapped_page(ctx, graftMap, atIndex + i, srcDoc, sourceStart + i);
        }
        pdf_drop_graft_map(ctx, graftMap);
    });
    pImpl->cachedPageCount += count;
    pImpl->markContentChanged();
    markNeedsFullGc();  // page import (graft) can duplicate resources → dedup
}

void Document::graftPagesPreservingArtifacts(int atIndex, const Document& source,
                                             int sourceStart, int sourceEnd) {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document");
    }
    if (!source.pImpl->pdfDoc) {
        throw PdfeException("Source is not a PDF document");
    }
    if (atIndex < 0 || atIndex > pImpl->cachedPageCount) {
        throw PdfeException("Insert index out of range: " + std::to_string(atIndex));
    }
    if (sourceStart < 0 || sourceEnd >= source.pImpl->cachedPageCount ||
        sourceStart > sourceEnd) {
        throw PdfeException("Source page range out of range");
    }

    auto* srcDoc = source.pImpl->pdfDoc;
    const int count = sourceEnd - sourceStart + 1;
    // Constructed BEFORE the fz_try frame inside execute() (MuPDF gotcha).
    std::vector<int> indices;
    indices.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) indices.push_back(sourceStart + i);

    pImpl->context->execute([this, atIndex, srcDoc, &indices](fz_context* ctx) {
        graftPagesWithArtifacts(ctx, pImpl->pdfDoc, srcDoc, indices, atIndex);
    });

    pImpl->cachedPageCount += count;
    pImpl->markContentChanged();
    markNeedsFullGc();  // page import (graft) can duplicate resources → dedup
}

void* Document::retainPageObj(int index) {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document");
    }
    if (index < 0 || index >= pImpl->cachedPageCount) {
        throw PdfeException("Page index out of range: " + std::to_string(index));
    }

    // Deep-copy the page into a standalone holder document so the retained
    // object is document-INDEPENDENT and survives a full-rewrite save: save()
    // drops+reopens pImpl->pdfDoc and saveAs() GC-renumbers it, either of which
    // would dangle a raw pdf_keep_obj into the live document → post-save undo =
    // UAF. The holder is re-grafted into whatever document is live at undo time
    // (Document::insertPageObj). See RetainedPageHolder above and
    // tests/test_undo_after_save_crash.cpp.
    return pImpl->context->executeReturn<void*>(
        [this, index](fz_context* ctx) -> void* {
            pdf_document* holder = pdf_create_document(ctx);
            if (!holder) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "pdf_create_document failed");
            }
            fz_try(ctx) {
                pdf_graft_page(ctx, holder, 0, pImpl->pdfDoc, index);
            } fz_catch(ctx) {
                pdf_drop_document(ctx, holder);
                fz_rethrow(ctx);
            }
            auto* h = new RetainedPageHolder{};
            h->doc = holder;
            h->context = pImpl->context;  // pin the context to the holder (B2)
            return h;
        }
    );
}

void Document::replacePage(int atIndex, const Document& source, int sourcePageIndex) {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document");
    }
    if (atIndex < 0 || atIndex >= pImpl->cachedPageCount) {
        throw PdfeException("Page index out of range: " + std::to_string(atIndex));
    }
    // Delete target page first, then graft source page at the same index.
    // Both operations validate their inputs and throw on failure.
    deletePage(atIndex);
    graftPage(atIndex, source, sourcePageIndex);
}

void Document::releasePageObj(void* obj) {
    // Delegate to the document-independent release so there is exactly one code
    // path (and so an accidental live-Document release stays consistent with the
    // teardown release used by command destructors).
    releaseRetainedPage(obj);
}

void Document::releaseRetainedPage(void* obj) {
    if (!obj) return;
    auto* holder = static_cast<RetainedPageHolder*>(obj);
    // Drop via the holder's OWN pinned context, not any Document — this is what
    // makes the release safe when the owning Document is already gone (B2).
    if (holder->doc && holder->context) {
        holder->context->execute([holder](fz_context* ctx) {
            pdf_drop_document(ctx, holder->doc);
        });
    }
    delete holder;
}

void* Document::createDisplayList(int pageIndex) {
    if (pageIndex < 0 || pageIndex >= pImpl->cachedPageCount) {
        throw PdfeException("Page index out of range: " + std::to_string(pageIndex));
    }

    std::lock_guard<std::mutex> lock(pImpl->docMutex);

    // The page's content stream may have been modified (e.g. by
    // pdf_filter_page_contents). No explicit cache drop is needed or possible:
    // pdf_drop_page_tree() is a no-op in MuPDF 1.26.10 and MuPDF invalidates
    // its own page map on the mutation. See
    // docs/audit/page-tree-flush-noop-2026-08-31.md.
    if (pImpl->dirtyPageIndices_.count(pageIndex) > 0) {
        pImpl->dirtyPageIndices_.erase(pageIndex);
    }

    // Load the page (guarded — see safeLoadPage).
    fz_page* fzPage = pImpl->context->executeReturn<fz_page*>(
        [this, pageIndex](fz_context* ctx) -> fz_page* {
            return safeLoadPage(ctx, pImpl->doc, pageIndex);
        }
    );

    fz_display_list* list = nullptr;

    // Check if we need to filter sticky notes
    if (pImpl->annotFilter_ == AnnotationFilter::HideStickyNotes && pImpl->pdfDoc) {
        // Build display list manually, skipping PDF_ANNOT_TEXT annotations
        try {
            list = pImpl->context->executeReturn<fz_display_list*>(
                [this, fzPage](fz_context* ctx) -> fz_display_list* {
                    // Get page bounds
                    fz_rect bounds = fz_bound_page(ctx, fzPage);

                    // Create empty display list
                    fz_display_list* dlist = fz_new_display_list(ctx, bounds);
                    fz_device* dev = nullptr;

                    // This used to be try / catch(...) / throw. That cleanup
                    // could NEVER run: every call below reports failure with
                    // fz_throw, which is longjmp, not a C++ exception -- so
                    // the catch was dead code and both dlist and dev leaked on
                    // any MuPDF error. The `throw;` was also a C++ exception
                    // crossing MuPdfContext::execute's own fz_try frame, which
                    // leaves MuPDF's error stack pushed. fz_var is required
                    // because dev is assigned after the setjmp.
                    fz_var(dev);
                    fz_try(ctx) {
                        // Create list device
                        dev = fz_new_list_device(ctx, dlist);

                        // Run page contents
                        fz_run_page_contents(ctx, fzPage, dev, fz_identity, nullptr);

                        // Get PDF page for annotation iteration
                        pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
                        if (pdfPage) {
                            // Iterate annotations, skipping PDF_ANNOT_TEXT
                            pdf_annot* annot = pdf_first_annot(ctx, pdfPage);
                            while (annot) {
                                int annotType = pdf_annot_type(ctx, annot);
                                if (annotType != PDF_ANNOT_TEXT) {
                                    pdf_run_annot(ctx, annot, dev, fz_identity, nullptr);
                                }
                                annot = pdf_next_annot(ctx, annot);
                            }

                            // Run widgets
                            fz_run_page_widgets(ctx, fzPage, dev, fz_identity, nullptr);
                        }

                        // Close the device inside the guarded block: closing
                        // is what flushes the list, and it can throw.
                        fz_close_device(ctx, dev);
                    }
                    fz_always(ctx) {
                        // fz_drop_device tolerates NULL, so this covers a
                        // failure of fz_new_list_device itself.
                        fz_drop_device(ctx, dev);
                    }
                    fz_catch(ctx) {
                        fz_drop_display_list(ctx, dlist);
                        fz_rethrow(ctx);
                    }

                    return dlist;
                }
            );
        } catch (...) {
            pImpl->context->execute([fzPage](fz_context* ctx) {
                fz_drop_page(ctx, fzPage);
            });
            throw;
        }
    } else {
        // Default: create display list from page (shows all annotations)
        try {
            list = pImpl->context->executeReturn<fz_display_list*>(
                [fzPage](fz_context* ctx) -> fz_display_list* {
                    return fz_new_display_list_from_page(ctx, fzPage);
                }
            );
        } catch (...) {
            pImpl->context->execute([fzPage](fz_context* ctx) {
                fz_drop_page(ctx, fzPage);
            });
            throw;
        }
    }

    // Drop the page — display list is self-contained
    pImpl->context->execute([fzPage](fz_context* ctx) {
        fz_drop_page(ctx, fzPage);
    });

    return list;
}

void Document::releaseDisplayList(void* displayList) {
    if (!displayList) return;
    auto* list = static_cast<fz_display_list*>(displayList);
    pImpl->context->execute([list](fz_context* ctx) {
        fz_drop_display_list(ctx, list);
    });
}

void Document::setAnnotationFilter(AnnotationFilter filter) {
    pImpl->annotFilter_ = filter;
}

AnnotationFilter Document::annotationFilter() const {
    return pImpl->annotFilter_;
}

void Document::markPageDirty(int pageIndex) {
    // ISSUE #125. Snapshot the listeners under the lock, then fire with the
    // lock RELEASED.
    //
    // Firing under docMutex would deadlock the moment a listener touched the
    // document it was told about — and a repaint listener is exactly the kind
    // that wants to (page counts, sizes, a re-render). std::mutex is not
    // recursive, so that is a hang, not an error. The copy is the price of not
    // holding a lock across a callback into code this class does not own.
    std::vector<PageDirtyListener> toCall;
    {
        std::lock_guard<std::mutex> lock(pImpl->docMutex);
        pImpl->dirtyPageIndices_.insert(pageIndex);
        toCall.reserve(pImpl->pageDirtyListeners_.size());
        for (const auto& entry : pImpl->pageDirtyListeners_) {
            toCall.push_back(entry.second);
        }
    }
    for (const auto& cb : toCall) {
        if (!cb) continue;
        // Swallowed for the same reason fireEvent swallows: a repaint hook must
        // not turn a successful edit into a failed one.
        try { cb(pageIndex); } catch (...) { }
    }
}

Document::PageDirtyToken Document::addPageDirtyListener(PageDirtyListener cb) {
    if (!cb) return 0;
    std::lock_guard<std::mutex> lock(pImpl->docMutex);
    const PageDirtyToken token = ++pImpl->nextPageDirtyToken_;
    pImpl->pageDirtyListeners_.emplace_back(token, std::move(cb));
    return token;
}

std::shared_ptr<const void> Document::lifetimeSentinel() const {
    return pImpl->lifetime_;
}

void Document::removePageDirtyListener(PageDirtyToken token) {
    if (token == 0) return;
    std::lock_guard<std::mutex> lock(pImpl->docMutex);
    auto& v = pImpl->pageDirtyListeners_;
    v.erase(std::remove_if(v.begin(), v.end(),
                           [token](const auto& e) { return e.first == token; }),
            v.end());
}

// --- F24: the certification (DocMDP) save guard ---
//
// One process-wide callback, installed by
// pdfe::signatures::installDocMdpSaveGuard(). See the seam's documentation in
// Document.h for why it is a seam and why it is process-wide.
namespace {
Document::SaveGuard g_saveGuard;
}

void Document::setSaveGuard(SaveGuard guard) {
    g_saveGuard = std::move(guard);
}

bool Document::hasSaveGuard() {
    return static_cast<bool>(g_saveGuard);
}

const std::string& Document::lastSaveGuardWarning() const {
    return pImpl->lastSaveGuardWarning_;
}

void Document::runSaveGuard(const std::filesystem::path& target) const {
    pImpl->lastSaveGuardWarning_.clear();
    if (!g_saveGuard) return;

    const bool inPlace = !pImpl->filePath.empty() && target == pImpl->filePath;
    SaveGuardVerdict verdict = g_saveGuard(this, target, inPlace);
    pImpl->lastSaveGuardWarning_ = verdict.message;
    if (!verdict.allow) {
        throw PdfeException(verdict.message.empty()
                                ? std::string("Save refused by the document "
                                              "certification guard")
                                : verdict.message);
    }
}

// --- GitHub #19 / PDF-50: the signed-rewrite guard ---

void Document::setSignedRewritePolicy(SignedRewritePolicy policy) {
    pImpl->signedRewritePolicy_ = policy;
}

Document::SignedRewritePolicy Document::signedRewritePolicy() const {
    return pImpl->signedRewritePolicy_;
}

int Document::appliedSignatureCount() const {
    if (!pImpl->pdfDoc) return 0;
    return pImpl->context->executeReturn<int>(
        [this](fz_context* ctx) -> int {
            // countAppliedSignatures never throws — it catches per traversal
            // step — so there is nothing here that could longjmp past a C++
            // object, and nothing non-trivially-destructible is built inside.
            return pdfe::core::countAppliedSignatures(ctx, pImpl->pdfDoc);
        });
}

int Document::appliedSignatureCountAtOpen() const {
    return pImpl->appliedSignaturesAtOpen_;
}

void Document::runSignedRewriteGuard(const std::filesystem::path& target) const {
    if (pImpl->signedRewritePolicy_ == SignedRewritePolicy::AcceptLoss) return;

    // needsFullGc_ is set ONLY by the destructive/structural operations
    // (redaction apply, page delete/reorder/import, form flatten, font
    // embedding, encryption change) and it implies requiresFullRewrite_, so
    // when it is set the incremental branch of save() is unreachable and a full
    // rewrite is certain. An additive-only session on a signed document does
    // not reach this test at all.
    if (!pImpl->needsFullGc_) return;

    // The GREATER of "what the file arrived with" and "what is reachable now".
    // The open-time count is what makes this work at all on the form-flatten
    // path (the bake has already orphaned the signature by now — see the
    // capture site in open()); the live count covers the documents a session
    // ADDED a signature to before making a destructive change.
    const int applied =
        std::max(pImpl->appliedSignaturesAtOpen_, appliedSignatureCount());
    if (applied <= 0) return;

    // Built outside any fz_try frame — appliedSignatureCount() has already
    // returned and MuPDF is not on the stack here.
    throw PdfeException(
        "Refusing to save: this would destroy "
        + std::to_string(applied)
        + (applied == 1 ? " existing digital signature."
                        : " existing digital signatures.")
        + "\n\nA destructive structural change (form flatten, annotation "
          "flatten, page delete/import, redaction apply, font embedding or an "
          "encryption change) has been made in this session, so \""
        + target.filename().string()
        + "\" must be written as a FULL REWRITE. That rebuilds the file from "
          "the object graph and moves every byte offset in it, so the "
          "/ByteRange each existing signature was computed over would no "
          "longer describe the file — and a form flatten additionally bakes "
          "the signature widget away entirely. Any validating reader would "
          "report the result as altered or corrupted.\n\nNothing was written; "
          "the document and the target file are unchanged. To proceed anyway "
          "and accept the loss, set the document's signed-rewrite policy to "
          "AcceptLoss.");
}

// --- Save Operations ---

void Document::save(const std::filesystem::path& path) {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document — cannot save");
    }

    // F24 — consult the certification guard BEFORE anything is written and
    // before willSave fires, so a refusal leaves the document and the target
    // file exactly as they were.
    runSaveGuard(path);

    // GitHub #19 / PDF-50 — same placement and the same reason: a refusal must
    // leave the document, the target file and the willSave listeners untouched.
    runSignedRewriteGuard(path);

    // GitHub #100 / WIN-78 — free-space pre-check. Placed after the refusal
    // guards (no point warning about a save that is about to be refused) and
    // before willSave, so the user hears about it before any listener has run
    // and before the multi-minute serialisation begins.
    //
    // ADVISORY ONLY. It does not branch, does not throw, and does not skip a
    // step; the save below runs identically whatever it reports. That is what
    // keeps TempWriteBlockModes/TempWriteFailureTest.
    // EnospcDuringTempWriteLeavesOriginalByteIdentical reaching the temp write
    // it exists to test.
    (void)pdfe::core::warnIfInsufficientSpaceForSave(path, pImpl->filePath);

    // Pipeline E phase-9: fire willSave so any document-level JS hooks
    // (e.g. updating a "last saved" form field) run before we serialize.
    pImpl->fireEvent("willSave");

    // Tier 1 incremental-save routing (pipeline b2e47d91): when the session
    // contains only additive edits (form-field fills, annotations) AND we are
    // saving back to the SAME file we opened from AND the in-memory object
    // numbering still matches that file, append the changes incrementally
    // instead of rewriting + GC'ing the whole document. For large dynamic-XFA
    // forms this turns a multi-minute save into a sub-second append.
    //
    //  - requiresFullRewrite_ guards confidentiality/structure: redaction,
    //    page ops, image/EXIF removal, etc. must scrub orphaned bytes via the
    //    full rewrite, which incremental append cannot do.
    //  - incrementalSafe_ guards correctness: after a saveAs() (GC renumber,
    //    no reopen) the in-memory numbering diverges from disk, so an append
    //    would corrupt the file.
    //  - path == filePath is REQUIRED, not just an optimization: saveIncremental
    //    always targets filePath; routing a save() aimed at a different path
    //    here would write to the wrong file.
    //
    // F58-c / F58-e (phase-53) — THIS BRANCH NOW STAGES, AND THE NUMBERS THAT
    // DECIDED IT.
    //
    // Phase-52 left this calling the UNSTAGED saveIncremental(), so the ordinary
    // File>Save route for additive edits — form fills and annotations, i.e.
    // exactly what users do to signed documents — kept the mid-write exposure
    // F58 had just called intolerable on the /DSS path. The stated reason was
    // the large-XFA performance win. The reviewer's counter-argument was that
    // staging is a FILE COPY, so it scales with SIZE, not with XFA complexity.
    //
    // MEASURED (this machine, Debug build, tests/fixtures/realworld/ssa_sf86.pdf,
    // 7,984,610 bytes — the largest XFA fixture in the corpus; see
    // StagedIncrementalXfaBenchmark.MeasureUnstagedVersusStagedAppend, which
    // prints these on every run so they cannot go stale silently):
    //
    //     saveIncremental()        2724 ms
    //     saveIncrementalStaged()  3555 ms
    //     staging overhead          831 ms   (Debug; ~10 MB/s copy under /RTC1)
    //
    // The reviewer is right and the phase-52 rationale does not survive contact
    // with the measurement. 831 ms of Debug-build file copy is not the thing the
    // incremental routing exists to avoid: the alternative it exists to avoid is
    // the gc=3 full rewrite, benchmarked at 314,536 ms on this same fixture
    // (tests/test_xfa_save_benchmark.cpp). The win is 314 s → 3.6 s instead of
    // 314 s → 2.7 s, and IncrementalSaveRoutingTest.FormFillSaveIsFastOnXfa's
    // 30 s budget is still met with an order of magnitude to spare.
    //
    // ON THE RE-OPEN. saveIncrementalStaged() drops and re-opens the document,
    // which saveIncremental() did not — so Page objects held across this call are
    // invalidated where they previously were not. That is not a new contract:
    // save()'s full-rewrite branch below has ALWAYS re-opened, and a caller
    // cannot predict which branch it gets (the choice depends on session state
    // it does not control), so any correct caller of save() already had to
    // tolerate it. This makes the two branches agree rather than diverge.
    if (!pImpl->requiresFullRewrite_
        && pImpl->incrementalSafe_
        && !pImpl->filePath.empty()
        && path == pImpl->filePath) {
        saveIncrementalStaged();    // appends to pImpl->filePath (== path)
        pImpl->fireEvent("didSave");
        return;
    }

    // On Windows, MuPDF cannot overwrite a file it has open (file locking).
    // Strategy: save to temp file, close document, rename temp over original, reopen.
    // WIN-87 (phase-56). The staging file is created HERE, behind an exclusive
    // handle, at the contractual "<path>.pdfe_tmp" name — and every later step
    // uses that handle rather than the name. See
    // writeFullRewriteThroughStagingHandle above for what this replaced and
    // why, and StagingFile::createFullRewriteTempFor for why this factory keeps
    // the predictable name (the R2-04 recovery contract) and what it does about
    // a pre-planted file or reparse point sitting at it.
    //
    // EXCEPTION SAFETY (phase-42 remediation, now discharged by the unique_ptr).
    // Everything between here and the publish can throw: the write (disk full,
    // write error), the fz_drop_document below, and the reopen in the failure
    // branch. MuPdfContext::execute() converts fz_throw into a C++ exception at
    // its boundary, outside the fz_catch, so ordinary RAII does unwind
    // correctly here (the setjmp/longjmp hazard applies INSIDE the lambdas, and
    // nothing here is inside one). `staging`'s destructor closes the handle and
    // removes the staging file on every throwing path, and — unlike the
    // phase-42 TempFileGuard it replaces — it cannot be defeated by a read-only
    // attribute, because the staging file is created fresh rather than copied.
    // On the SUCCESS path commitOver has already renamed it away and the
    // destructor removes nothing. A crash never runs any of this, so the
    // orphan-".pdfe_tmp"-at-open recovery contract is unaffected.
    std::error_code stageEc;
    std::unique_ptr<StagingFile> staging =
        StagingFile::createFullRewriteTempFor(path, stageEc);
    if (!staging) {
        throw PdfeException(
            "Could not create the staging file for " + path.string()
            + " (original left intact): " + stageEc.message());
    }

    // NO PAGE-TREE FLUSH HERE, DELIBERATELY. There used to be a
    // pdf_drop_page_tree() call at this point, commented "flush cached page
    // tree so pdf_save_document writes all in-memory modifications". That
    // comment was false and the call did nothing: in MuPDF 1.26.10
    // pdf_drop_page_tree() is `{ /* Historical entry point. Now does nothing.
    // We drop 'just in time'. */ }` (pdf-page.c:152). MuPDF invalidates the
    // page map itself inside the object-mutation path (pdf-object.c:1818) and
    // on journal undo/redo, repair and xref reload. See
    // docs/audit/page-tree-flush-noop-2026-08-31.md — the whole family of nine
    // such calls was removed together, and PageTreeFlush.* pins the property
    // the comments had been claiming.

    // Clear dirty page set since we're about to save everything
    {
        std::lock_guard<std::mutex> lock(pImpl->docMutex);
        pImpl->dirtyPageIndices_.clear();
    }

    // 1. Save to temp file. Tier 2 (pipeline b2e47d91): use gc=3 only when a
    // dedup-worthy op occurred this session; otherwise the cheap default level
    // (still scrubs unreferenced/orphaned objects, just skips the expensive
    // dedup pass).
    const int gcLevel = effectiveGarbageLevel();
    // F14 (PDF-01): never emit a header EARLIER than the document's version.
    applyVersionFloorForSave();
    {
        pdf_write_options opts{};
        opts.do_garbage = gcLevel;
        opts.do_compress = 1;
        // do_compress_images / do_compress_fonts are SAFE to leave on for every
        // save (pipeline 3f8a1c72 Phase 1, verified against MuPDF 1.26.10 source):
        // MuPDF's copystream() (pdf-write.c:~809) only deflates a stream that has
        // NO existing /Filter. Any image already carrying /DCTDecode, /JPXDecode,
        // /FlateDecode, /CCITTFaxDecode, or /JBIG2Decode keeps its filter and is
        // copied byte-for-byte — there is no JPEG/DCT re-encoder anywhere in the
        // write path, so "compress images" can only Flate filterless raw data and
        // never lossily re-encodes an already-compressed image. Likewise there is
        // no font-subsetting in the write path (pdf_subset_fonts lives only in
        // pdf-clean-file.c), so an already-subset (XXXXXX+) font is never
        // re-subset. A PDFE-side per-object guard would only duplicate this.
        opts.do_compress_images = 1;
        opts.do_compress_fonts = 1;
        opts.do_preserve_metadata = 1;
        // PDF-02: keep the compact on-disk shape the document arrived with. 0
        // for a classic-xref source — see effectiveObjectStreamMode(), which
        // preserves and never imposes.
        opts.do_use_objstms = effectiveObjectStreamMode();
        writeFullRewriteThroughStagingHandle(*pImpl->context, pImpl->pdfDoc,
                                             *staging, opts);
    }

    // 2. Close the current document handle to release file lock
    pImpl->context->execute([this](fz_context* ctx) {
        fz_drop_document(ctx, pImpl->doc);
    });
    pImpl->doc = nullptr;
    pImpl->pdfDoc = nullptr;

    // 3. Flush the just-written temp to stable storage — THROUGH THE EXCLUSIVE
    //    HANDLE (WIN-87 criterion 2). Its bytes may still sit in the OS
    //    write-back cache; a crash right after the rename could then leave the
    //    directory entry pointing at unflushed data. Best-effort, every save.
    //
    //    This used to be flushFileToStableStorage(tempPath), which re-opened
    //    the staging file BY NAME (CreateFileW/OPEN_EXISTING). That was the
    //    second of the three name resolutions this phase removes, and it was
    //    the easiest one to miss: it is one line, it looks like durability
    //    plumbing rather than a security boundary, and closing only the rename
    //    would have left it — and the write — wide open.
    {
        std::error_code flushEc;
        staging->flushToStableStorage(flushEc);  // best-effort, as before
    }

    // 4. Atomically replace the original with the temp. This NEVER removes the
    //    original first. The prior remove()+rename() left a window in which a
    //    crash — or a rename failure at the second step — destroyed the user's
    //    file leaving only the temp (silent data loss, R2-04). An atomic
    //    rename-over keeps the original byte-intact until the instant it is
    //    replaced, and on ANY failure the original is still on disk.
    //
    //    Keep the 10×/100ms retry to tolerate brief post-fz_drop_document file
    //    handle holdovers on Windows — but retry the RENAME-OVER, not a remove.
    //    F10/WIN-84: that retry is ALSO the anti-A/V window — a scanner or the
    //    search indexer routinely holds a just-written file open for a few
    //    hundred milliseconds. See core/AtomicFileReplace.h.
    //
    //    Published THROUGH the exclusive handle (WIN-87): the staging path is
    //    never re-resolved by name between creation and publication, so there
    //    is no instant at which another process can substitute what gets
    //    published. commitOverWithRetry keeps the 10×/100 ms calibration the
    //    by-path helper carried.
    std::error_code ec;
    const bool replaced = staging->commitOverWithRetry(path, ec);

    if (!replaced) {
        // The original was NEVER removed — it is still byte-intact on disk. Reopen
        // it so the Document stays fully usable (pImpl->doc/pdfDoc valid, not a
        // dead null handle), clean up the orphan temp best-effort, then surface
        // the failure. The caller sees the save failed AND still holds a live
        // Document over the unchanged original — no lost changes, no dead object.
        //
        // The orphan temp is removed by `staging`'s destructor (phase-56; the
        // phase-42 TempFileGuard it replaces did the same job), so it is
        // cleaned up however this branch exits — including when the reopen
        // below throws, and including the throwing paths ABOVE this branch that
        // never reach it at all. An earlier phase-42 attempt used a bare
        // remove() placed here instead; that closed only the path a test
        // happened to drive.
        //
        // Found by RedactionApplyTest.NoFilesystemResidueFromAnyApplyPath, the
        // class-level directory-diff oracle — the two suffix-keyed sweeps that
        // guarded DCA 0007 and 0010 look only for `.redact_tmp`/`.redact_out`
        // and are structurally blind to a `.pdfe_tmp`.
        std::string reopenStr = mupdfPathUtf8(path);
        pImpl->doc = pImpl->context->executeReturn<fz_document*>(
            [&reopenStr](fz_context* ctx) -> fz_document* {
                return fz_open_document(ctx, reopenStr.c_str());
            }
        );
        pImpl->pdfDoc = pImpl->context->executeReturn<pdf_document*>(
            [this](fz_context* ctx) -> pdf_document* {
                return pdf_specifics(ctx, pImpl->doc);
            }
        );
        pImpl->cachedPageCount = pImpl->context->executeReturn<int>(
            [this](fz_context* ctx) -> int {
                return fz_count_pages(ctx, pImpl->doc);
            }
        );
        // `staging` still holds its exclusive handle; its destructor closes it
        // and removes the orphan as this throw unwinds. (An earlier revision
        // retried the removal here and justified it by a handle "held over the
        // temp" by the reopen — that rationale was simply wrong: the reopen
        // opens `path`, not the staging file, and fz_drop_document has already
        // run.)
        throw PdfeException(
            "Failed to atomically replace original file (original left "
            "intact): " + ec.message());
    }

    // The replace succeeded: the staging file no longer exists under its own
    // name (it IS `path` now) and commitOver has already marked itself
    // committed, so the destructor removes nothing.


    // 5. Reopen from the saved file
    std::string pathStr = mupdfPathUtf8(path);
    pImpl->doc = pImpl->context->executeReturn<fz_document*>(
        [&pathStr](fz_context* ctx) -> fz_document* {
            return fz_open_document(ctx, pathStr.c_str());
        }
    );
    pImpl->pdfDoc = pImpl->context->executeReturn<pdf_document*>(
        [this](fz_context* ctx) -> pdf_document* {
            return pdf_specifics(ctx, pImpl->doc);
        }
    );
    pImpl->cachedPageCount = pImpl->context->executeReturn<int>(
        [this](fz_context* ctx) -> int {
            return fz_count_pages(ctx, pImpl->doc);
        }
    );

    pImpl->modified = false;
    // A clean full-rewrite + reopen just baked a GC'd file and resynced the
    // in-memory numbering to disk: destructive edits are scrubbed, so the doc
    // is once again incremental-eligible. The reopen synced us to `path`, but
    // saveIncremental() targets filePath — so we are only incremental-safe for
    // the next save when this save wrote back to filePath itself (the normal
    // File>Save case). A save() to some other path leaves us synced to a file
    // that is NOT filePath, so keep incremental disabled there.
    pImpl->requiresFullRewrite_ = false;
    pImpl->needsFullGc_ = false;  // the dedup just ran (if it was needed)
    pImpl->incrementalSafe_ = (path == pImpl->filePath);
    pImpl->fireEvent("didSave");
}

// ---------------------------------------------------------------------------
// WIN-87 site #3, phase-57. See the contract in Document.h — this comment is
// only about the ORDER, which is load-bearing in three places.
// ---------------------------------------------------------------------------
void Document::saveThroughExternalRewrite(
    const std::function<void(std::FILE* in, std::FILE* out)>& rewrite)
{
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document — cannot save");
    }
    if (!rewrite) {
        throw PdfeException(
            "saveThroughExternalRewrite requires a rewrite callback");
    }
    const std::filesystem::path target = pImpl->filePath;
    if (target.empty()) {
        throw PdfeException(
            "Cannot rewrite a document that was not opened from a file path");
    }

    // F24 — the guard is consulted for the REAL target, before anything is
    // written and before willSave fires, so a refusal leaves both the document
    // and the file exactly as they were.
    runSaveGuard(target);
    pImpl->fireEvent("willSave");

    std::error_code stageEc;
    std::unique_ptr<StagingFile> bake = StagingFile::createBeside(target, stageEc);
    if (!bake) {
        throw PdfeException(
            "Could not create the bake staging file for " + target.string()
            + " (original left intact): " + stageEc.message());
    }
    std::unique_ptr<StagingFile> out = StagingFile::createBeside(target, stageEc);
    if (!out) {
        throw PdfeException(
            "Could not create the output staging file for " + target.string()
            + " (original left intact): " + stageEc.message());
    }

    // Clear the dirty set since we are about to write everything.
    // No page-tree flush here: pdf_drop_page_tree() is a documented no-op
    // in MuPDF 1.26.10 (pdf-page.c:152) and MuPDF invalidates the page map
    // itself. See docs/audit/page-tree-flush-noop-2026-08-31.md.
    {
        std::lock_guard<std::mutex> lock(pImpl->docMutex);
        pImpl->dirtyPageIndices_.clear();
    }

    const int gcLevel = effectiveGarbageLevel();
    applyVersionFloorForSave();
    {
        pdf_write_options opts{};
        opts.do_garbage = gcLevel;
        opts.do_compress = 1;
        opts.do_compress_images = 1;
        opts.do_compress_fonts = 1;
        opts.do_preserve_metadata = 1;
        writeFullRewriteThroughStagingHandle(*pImpl->context, pImpl->pdfDoc,
                                             *bake, opts);
    }

    // ORDER POINT 1. Drop the live document handle BEFORE the publish, and do
    // it here rather than after the rewrite so there is one drop on every path.
    // MuPDF opens through the CRT's fopen("rb"), which requests no
    // FILE_SHARE_DELETE, so a still-open document on `target` makes the
    // rename-over below fail outright.
    pImpl->context->execute([this](fz_context* ctx) {
        fz_drop_document(ctx, pImpl->doc);
    });
    pImpl->doc = nullptr;
    pImpl->pdfDoc = nullptr;

    // Reopen onto `where`. Used by every failure branch below so a failed
    // rewrite leaves a fully usable Document rather than a dead null handle.
    // Best-effort by design: it must not replace the failure the caller
    // actually needs to hear about.
    //
    // TWO THINGS HERE ARE DELIBERATE AND WERE BOTH LEARNED THE EXPENSIVE WAY.
    //
    // (1) THE HANDLER IS NAMED EXPLICITLY RATHER THAN SNIFFED FROM THE
    //     FILENAME. `fz_open_document(ctx, name)` picks its handler by
    //     RECOGNISING the name — which works for "*.pdf" and cannot work for a
    //     StagingFile, whose whole security property is that it is called
    //     "<doc>.pdfe_part.<32 hex>". The bake reopen below is exactly that
    //     case, so the by-name form fails on a file that is a perfectly valid
    //     PDF. That failure is silent (see (2)) and its consequence is severe:
    //     the fallback then reopens the ORIGINAL, and a redaction the user was
    //     told to expect is replaced by the un-redacted document.
    //     `fz_open_document_with_stream` takes the magic as an argument, so the
    //     name stops being load-bearing. Nothing here should ever have depended
    //     on a filename spelling.
    //
    // (2) THE FAILURE IS NO LONGER SWALLOWED WHOLE. This used to be a bare
    //     `catch (...) {}`, which is why the bake-reopen defect could not be
    //     instrumented at all — the one place that knew why the reopen failed
    //     discarded it. The handles are still left null rather than the
    //     exception propagated (the caller is already being told the rewrite
    //     failed, and the original is untouched on disk either way), but the
    //     reason is captured so the throw below can carry it.
    std::string reopenFailure;
    // THE BY-NAME REOPEN HELPER WAS DELETED, not left unused. Its last caller
    // was the double-failure fallback below, and that caller WAS the
    // confidentiality defect: reopening `target` there hands back the
    // un-redacted original. Every other reopen in this method goes through the
    // handle it already holds. Keeping a by-name reopen sitting here for a
    // future caller would be keeping the defect's mechanism within reach.

    // Reopen from an ALREADY-OPEN stream. Used for the retained bake below,
    // where opening by name is not merely riskier but impossible — see that
    // branch. `fz_open_file_ptr_no_close` does NOT take ownership of `fp`, so
    // the caller owns it and must keep it alive for the document's lifetime;
    // the fz_stream we create here IS owned by the document once
    // fz_open_document_with_stream succeeds, so we drop our own reference.
    auto reopenFromStream = [this, &reopenFailure](std::FILE* fp) noexcept {
        try {
            pImpl->doc = pImpl->context->executeReturn<fz_document*>(
                [fp](fz_context* ctx) -> fz_document* {
                    fz_stream* s = NULL;
                    fz_document* d = NULL;
                    fz_var(s);
                    fz_var(d);
                    fz_try(ctx) {
                        std::fseek(fp, 0, SEEK_SET);
                        s = fz_open_file_ptr_no_close(ctx, fp);
                        d = fz_open_document_with_stream(
                                ctx, "application/pdf", s);
                    }
                    fz_always(ctx) {
                        fz_drop_stream(ctx, s);
                    }
                    fz_catch(ctx) {
                        fz_rethrow(ctx);
                    }
                    return d;
                });
            pImpl->pdfDoc = pImpl->context->executeReturn<pdf_document*>(
                [this](fz_context* ctx) -> pdf_document* {
                    return pdf_specifics(ctx, pImpl->doc);
                });
            pImpl->cachedPageCount = pImpl->context->executeReturn<int>(
                [this](fz_context* ctx) -> int {
                    return fz_count_pages(ctx, pImpl->doc);
                });
        } catch (const std::exception& e) {
            reopenFailure += std::string(" [reopen from the retained bake "
                                         "stream failed: ") + e.what() + "]";
        } catch (...) {
            reopenFailure += " [reopen from the retained bake stream failed: "
                             "unknown exception]";
        }
    };

    // RETAIN THE BAKE AND REOPEN ONTO IT. Used by BOTH failure branches below,
    // and the fact that it is shared is the fix for a real defect: the two
    // branches were written separately, one of them reopened the UN-REDACTED
    // ORIGINAL, and its comment justified that with "nothing has been
    // published, so the ORIGINAL is the correct state to return to". That
    // premise is wrong in the same way the first draft of the other branch was.
    // By the time either is reached the bake IS WRITTEN and the redacted
    // in-memory document HAS BEEN DROPPED, so reopening the original silently
    // hands the caller back the confidential content it just removed — while
    // applyAllRedactions still returns success == true, because
    // `documentRewritten` has no production consumer. Found by a non-gate
    // security review AFTER the identical defect had been found and fixed on
    // the sibling branch sixty lines away: the recurring shape in this project
    // is a correct fix with an unfixed twin nearby.
    //
    // Reopening `target` BY NAME would be correct only for failures BEFORE the
    // bake is written (the save guard, either createBeside) — and those all
    // return earlier, which is why no such helper exists here any more.
    auto retainBakeAndReopen = [this, &bake, &reopenFromStream,
                                &reopenFailure, &target]() noexcept {
        std::error_code dupEc;
        std::FILE* bakeStream = bake ? bake->openDuplicateStream(dupEc) : nullptr;
        // Release any retention from an EARLIER failed publish before taking a
        // new one. The document that was reading that stream was dropped above,
        // so this is the safe moment; without it a second consecutive failure
        // would overwrite the retained pointers and leak the previous stream
        // (and orphan the previous staging file's deletion).
        pImpl->releaseRetainedRewrite();
        if (bake) pImpl->retainedRewriteStaging_ = std::move(bake);
        if (bakeStream) {
            reopenFromStream(bakeStream);
        } else {
            reopenFailure += " [could not duplicate the bake staging handle: "
                           + dupEc.message() + "]";
        }
        if (!pImpl->doc) {
            // THE BAKE COULD NOT BE REOPENED EITHER, AND THE DOCUMENT IS LEFT
            // CLOSED RATHER THAN REOPENED ONTO `target`.
            //
            // Falling back to the original here is the intuitive answer and it
            // is the SAME CONFIDENTIALITY DEFECT this method already shipped
            // and reverted once, one nesting level down: the caller would be
            // handed back a live Document holding the UN-REDACTED content while
            // `applyAllRedactions` still reported `success == true` (only
            // `documentRewritten` goes false, and no caller in the tree inspects
            // it), and a later saveAs() would write the secret out. Found by the
            // phase-4 security-audit gate, in the sub-case the first fix did not
            // reach. This programme's recurring shape: the sibling defect is
            // never in the other file, it is a few lines further down the one
            // just corrected.
            //
            // A closed Document is the fail-CLOSED outcome. Both call sites
            // throw immediately after this returns, so the caller learns why;
            // what it cannot do is silently save unredacted bytes.
            if (bakeStream) { std::fclose(bakeStream); bakeStream = nullptr; }
            pImpl->retainedRewriteStaging_.reset();
            reopenFailure +=
                " [the Document was left CLOSED rather than reopened onto the "
                "un-redacted original, which would have discarded the redaction "
                "while still reporting success]";
        } else {
            pImpl->retainedRewriteStream_ = bakeStream;
        }
    };

    // ORDER POINT 2. The callback runs OUTSIDE any fz_try region — nothing with
    // a C++ destructor may be constructed inside one (setjmp/longjmp does not
    // unwind), and this whole block is full of them.
    try {
        std::error_code dupEc;
        std::FILE* in = bake->openDuplicateStream(dupEc);
        if (!in) {
            throw PdfeException(
                "Could not open a read stream over the staged bake (original "
                "left intact): " + dupEc.message());
        }
        std::FILE* os = out->openDuplicateStream(dupEc);
        if (!os) {
            std::fclose(in);
            throw PdfeException(
                "Could not open the output staging stream (original left "
                "intact): " + dupEc.message());
        }
        // Ownership of BOTH streams passes to the callback here, on every path
        // including a throwing one. See the contract in Document.h.
        rewrite(in, os);
    } catch (...) {
        // THE BAKE IS ALREADY WRITTEN AND THE REDACTED DOCUMENT ALREADY
        // DROPPED, so reopening the ORIGINAL here would hand the caller back
        // the confidential content the redaction just removed — with
        // applyAllRedactions still reporting success. This branch used to do
        // exactly that. The transform failing means the OUTPUT is unusable; it
        // says nothing about the bake, which is the redaction.
        retainBakeAndReopen();
        throw;
    }

    {
        std::error_code flushEc;
        out->flushToStableStorage(flushEc);  // best-effort, as in save()
    }

    // ORDER POINT 3. Publish THROUGH the exclusive handle, keeping the
    // 10×/100 ms anti-A/V calibration (F10/WIN-84) that the by-path helper
    // carried. On ANY failure the original is still byte-intact — this never
    // removes it first.
    std::error_code ec;
    if (!out->commitOverWithRetry(target, ec)) {
        // ---------------------------------------------------------------
        // THE FAILURE BRANCH IS A CONFIDENTIALITY BOUNDARY HERE, NOT JUST A
        // TIDY-UP — and getting it wrong produced a real regression that
        // `Jbig2RedactionTest.OriginalJbig2BytesAreNotRecoverable` caught.
        //
        // The obvious fallback is to reopen `target` by name: the original is
        // byte-intact, so reopening it leaves a usable Document over the
        // user's file. For an ordinary save that is right. For THIS method it
        // is wrong, because the staged bytes are a REDACTION: reopening the
        // original silently discards the scrubbing that was just performed and
        // hands the caller a document whose confidential content is back. A
        // later saveAs() then writes the unredacted file out, which is exactly
        // how the JBIG2 case failed — "success" from applyAllRedactions, and
        // the coded scan recoverable from the saved file.
        //
        // So we reopen onto the BAKE instead. It contains precisely the
        // in-memory state that was about to be published (full rewrite, GC'd),
        // so the Document keeps the redaction and only the PUBLICATION failed —
        // which is what `documentRewritten == false` plus this error message
        // report. This also restores the pre-phase-57 behaviour exactly: the
        // old by-path relay reopened the live document onto "<doc>.redact_tmp"
        // as a side effect of baking, and every caller has depended on that
        // since #46 without it being written down anywhere.
        //
        // AND IT IS REOPENED THROUGH THE HANDLE, NOT BY NAME. Two reasons, and
        // the first one is a defect this fallback actually had:
        //
        //   * BY NAME IT CANNOT WORK AT ALL. The StagingFile holds its own
        //     handle with FILE_SHARE_READ only. MuPDF's by-name open goes
        //     through the CRT, which requests share read AND write — and that
        //     conflicts with our own reservation, so the open fails with a
        //     sharing violation every single time. The fallback then dropped to
        //     reopening the ORIGINAL, i.e. the un-redacted document, which is
        //     exactly the confidentiality regression this branch exists to
        //     prevent. It failed silently because the reason was discarded by a
        //     bare `catch (...)` (now fixed above). PDFE denying ITSELF access
        //     to its own staging file is the same self-inflicted shape as the
        //     live-Page publish failure that gets us into this branch.
        //
        //   * BY HANDLE THERE IS NO NAME TO WIN. Reopening by name would need
        //     the StagingFile kept alive purely so nobody could substitute the
        //     file between our decision and MuPDF's open — re-creating window A
        //     in this method's own error path. A duplicate of the handle we
        //     already hold has no such interval.
        //
        // The duplicate stream must outlive the fz_document (MuPDF reads
        // lazily), and fz_open_file_ptr_no_close does not take ownership, so
        // both the StagingFile and the FILE* are retained on the Impl and
        // released together. The StagingFile's destructor deletes the file when
        // the Document is destroyed — the #46 contract, now enforced by an
        // object rather than by a remembered filename.
        // ---------------------------------------------------------------
        retainBakeAndReopen();
        // The in-memory numbering now matches the BAKE, not filePath(), so a
        // later incremental append onto filePath() would corrupt it.
        pImpl->incrementalSafe_ = false;
        pImpl->requiresFullRewrite_ = true;

        // NAME THE LIKELY CAUSE. "Access is denied" on this specific operation
        // is almost always the caller holding a live Page across the call: a
        // Page keeps an fz_keep_document reference (MuPDF's fz_load_page), so
        // the fz_drop_document above did not close the file and this process is
        // denying its own rename. A bare errno sends a maintainer looking at
        // permissions, which is the wrong file entirely. The caller-side fix is
        // the drain+rebind contract in core/SaveWithRebind.h
        // (drainRebindAroundWrite), or MainWindow::onApplyRedactions's
        // P11-BUG-1 sequence; Document cannot drain Pages it does not own.
        throw PdfeException(
            "Failed to atomically replace the original with the rewritten "
            "document (original left intact, and this Document now reads the "
            "rewritten content from its staging file so the transform is not "
            "lost): " + ec.message()
            + ". If this is an access/sharing failure, no Page object (or "
              "tile/display-list holder) may be alive across this call: a live "
              "Page keeps the document handle open and this process then "
              "blocks its own publish. See core/SaveWithRebind.h's "
              "drainRebindAroundWrite for the caller-side fix."
            + reopenFailure);
    }

    // Published: any previously retained staging file is no longer what this
    // Document reads, so let it go (its destructor removes it). The stream goes
    // with it — the document that read through it was dropped before the commit
    // above, so nothing is still reading when this closes.
    pImpl->releaseRetainedRewrite();

    // Reopen from the published file. A failure here is NOT a failed save: the
    // new bytes are on disk. Surface it, but only after the Document has been
    // put into a defined state.
    std::string pathStr = mupdfPathUtf8(target);
    pImpl->doc = pImpl->context->executeReturn<fz_document*>(
        [&pathStr](fz_context* ctx) -> fz_document* {
            return fz_open_document(ctx, pathStr.c_str());
        });
    pImpl->pdfDoc = pImpl->context->executeReturn<pdf_document*>(
        [this](fz_context* ctx) -> pdf_document* {
            return pdf_specifics(ctx, pImpl->doc);
        });
    pImpl->cachedPageCount = pImpl->context->executeReturn<int>(
        [this](fz_context* ctx) -> int {
            return fz_count_pages(ctx, pImpl->doc);
        });

    pImpl->modified = false;
    // Same reasoning as save()'s full-rewrite tail: a clean full rewrite plus
    // reopen scrubbed the destructive edits and resynced the in-memory
    // numbering to disk, and we wrote back to filePath itself. The external
    // transform renumbered objects wholesale (QPDF's rewrite does), so the
    // reopen — not the pre-write state — is what makes this true.
    pImpl->requiresFullRewrite_ = false;
    pImpl->needsFullGc_ = false;
    pImpl->incrementalSafe_ = true;
    pImpl->fireEvent("didSave");

    // `bake` is destroyed here: its handle closes and its file is removed. It
    // was never published, never named predictably, and never unheld.
}

// WIN-87 phase-59 — the compliance converters. See Document.h for the contract
// and for what this closes; the short version is that those sites ran the
// pre-phase-57 three-step by-path relay AND had no atomic publish at all, so a
// failure mid-write truncated the user's output.
//
// This deliberately shares NONE of saveThroughExternalRewrite's reopen
// machinery. That function republishes over filePath() and must therefore drop
// and reopen the live handle; here the source document stays open and untouched
// while a DIFFERENT path is written, which is saveAs semantics. The one thing
// both must do identically is never resolve a staging name twice.
void Document::saveAsThroughExternalRewrite(
    const std::filesystem::path& target,
    const std::function<void(std::FILE* in, std::FILE* out)>& rewrite)
{
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document — cannot save");
    }
    if (!rewrite) {
        throw PdfeException(
            "saveAsThroughExternalRewrite requires a rewrite callback");
    }
    if (target.empty()) {
        throw PdfeException("saveAsThroughExternalRewrite requires a target");
    }

    // The F24 guard is consulted for the REAL target, before anything is
    // written — a refusal must leave both documents exactly as they were.
    runSaveGuard(target);

    // GitHub #19 / PDF-50. This route is a full external rewrite by
    // construction, so a destructive session on a signed document reaches the
    // same outcome here as through saveAs() and must be refused in the same
    // place. It fires only when needsFullGc() is set, so an ordinary PDF/A
    // conversion of an untouched signed document is unaffected.
    runSignedRewriteGuard(target);

    std::error_code stageEc;
    std::unique_ptr<StagingFile> bake = StagingFile::createBeside(target, stageEc);
    if (!bake) {
        throw PdfeException(
            "Could not create the bake staging file for " + target.string()
            + " (target left intact): " + stageEc.message());
    }
    std::unique_ptr<StagingFile> out = StagingFile::createBeside(target, stageEc);
    if (!out) {
        throw PdfeException(
            "Could not create the output staging file for " + target.string()
            + " (target left intact): " + stageEc.message());
    }

    // No page-tree flush here: pdf_drop_page_tree() is a documented no-op
    // in MuPDF 1.26.10 (pdf-page.c:152) and MuPDF invalidates the page map
    // itself. See docs/audit/page-tree-flush-noop-2026-08-31.md.

    {
        pdf_write_options opts{};
        opts.do_garbage = effectiveGarbageLevel();
        opts.do_compress = 1;
        // do_compress_images / do_compress_fonts: safe per the same MuPDF
        // /Filter guard documented at save() above (pipeline 3f8a1c72 Phase 1) —
        // already-compressed images and fonts are copied verbatim, only
        // filterless streams are Flate-compressed, and nothing here is lossy.
        opts.do_compress_images = 1;
        opts.do_compress_fonts = 1;
        opts.do_preserve_metadata = 1;
        // THE BAKE IS WRITTEN UNENCRYPTED, DELIBERATELY. Leaving this line out
        // was a regression I introduced in the migration and caught by
        // re-reading the diff against the code it replaced — the same species
        // this pipeline has hit in every phase since 56.
        //
        // pdf_write_options{} zero-initialises do_encrypt to PDF_ENCRYPT_KEEP,
        // so an encrypted source is re-encrypted into the bake, and the rewrite
        // callback then receives a stream it cannot read without the password
        // (QPDF throws "invalid password") — turning a conversion that used to
        // succeed into a failure. BOTH converters this API exists for set
        // PDF_ENCRYPT_NONE explicitly for exactly that reason, and PDF/A
        // forbids encryption outright (rule 6.1.3; analyzeForConversion already
        // warns "Encryption will be removed during conversion").
        //
        // WHAT WRITING IT IN THE CLEAR ACTUALLY COSTS, stated at the strength
        // the evidence supports. The first version of this comment claimed it
        // "exposes nothing to anyone who could not already read the decrypted
        // document out of this process". THAT IS TOO STRONG, and the phase-6
        // security-audit gate said so: createBeside opens with
        // FILE_SHARE_READ, so a local principal who can LIST the target's
        // directory sees the staging name the moment it appears
        // (FindFirstFile / ReadDirectoryChangesW) and can open it for read
        // during the window — the CSPRNG name defeats GUESSING, not
        // ENUMERATION. For a large conversion that window is not brief.
        //
        // It is still the right trade here, and it is a NARROWING rather than
        // a new exposure: the code this replaced wrote the identical
        // unencrypted bake (it set PDF_ENCRYPT_NONE too) to a fully
        // PREDICTABLE name, unheld, writable and deletable by anyone who could
        // reach the directory. PDF/A forbids encryption outright, so the
        // published output is unencrypted by design; the only question is the
        // intermediate, and it is now unguessable and unwritable rather than
        // neither.
        //
        // FOLLOW-UP, recorded rather than assumed away: giving these bakes a
        // restrictive owner-only DACL at creation would close the enumeration
        // vector outright, instead of relying on the directory's inherited
        // ACL. That belongs to StagingFile, not to this call site.
        //
        // What the CALLBACK writes is what gets published, and the callback
        // owns that decision.
        opts.do_encrypt = PDF_ENCRYPT_NONE;
        writeFullRewriteThroughStagingHandle(*pImpl->context, pImpl->pdfDoc,
                                             *bake, opts);
    }

    // Ownership of BOTH streams passes to the callback on every path,
    // including a throwing one — same contract as saveThroughExternalRewrite.
    {
        std::error_code dupEc;
        std::FILE* in = bake->openDuplicateStream(dupEc);
        if (!in) {
            throw PdfeException(
                "Could not open a read stream over the staged bake (target "
                "left intact): " + dupEc.message());
        }
        std::FILE* os = out->openDuplicateStream(dupEc);
        if (!os) {
            std::fclose(in);
            throw PdfeException(
                "Could not open the output staging stream (target left "
                "intact): " + dupEc.message());
        }
        rewrite(in, os);
    }

    {
        std::error_code flushEc;
        out->flushToStableStorage(flushEc);  // best-effort, as in save()
    }

    // Publish THROUGH the exclusive handle. Nothing is removed first, so on any
    // failure `target` is byte-intact and both staging files die with their
    // owners. This is the step the converters did not have AT ALL: they wrote
    // QPDFWriter output straight over the user's path.
    std::error_code ec;
    if (!out->commitOverWithRetry(target, ec)) {
        throw PdfeException(
            "Failed to publish the rewritten document over " + target.string()
            + " (target left intact): " + ec.message());
    }

    // `bake` and `out` are destroyed here. Neither was ever published under a
    // guessable name, and neither was ever unheld. This Document is untouched:
    // still open on its own file, still at its own filePath().
}

void Document::saveAs(const std::filesystem::path& path) {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document — cannot save");
    }

    // Pipeline f7c2a4e8 phase-2 (A5 / GAP-51 follow-up): reject obviously
    // dangerous save paths before touching the filesystem. The concerns:
    //   - Embedded null bytes: legacy C-string truncation; std::filesystem
    //     accepts them but the underlying CRT fopen() truncates at the
    //     first NUL, silently writing to a different file than the caller
    //     intended.
    //   - Windows alternate data streams (file.pdf:stream): rare and
    //     surprising; legitimate uses are essentially zero for PDF saves.
    //     A malicious caller could hide data in a stream of the same
    //     filename, breaking the user's mental model of "file" identity.
    //   - Empty path: would create a file named "" or pass NULL to fopen.
    // Parent-traversal (../../) is NOT rejected here — that may be a
    // legitimate user-chosen path (e.g. saving to a sibling directory).
    // The dialog layer is the right place to enforce path constraints
    // for user-chosen saves; saveAs() only blocks objectively-broken
    // inputs.
    // Cross-platform path safety checks. native() returns wstring on
    // Windows (wchar_t) and string on Linux (char), so use the
    // platform-appropriate types. Alternate-data-stream check applies
    // only to Windows NTFS.
    {
        const auto& native_path = path.native();
        using native_char = std::filesystem::path::value_type;
        if (native_path.empty()) {
            throw PdfeException("Refusing to save: empty path");
        }
        if (native_path.find(static_cast<native_char>('\0'))
                != std::filesystem::path::string_type::npos) {
            throw PdfeException("Refusing to save: path contains embedded null byte");
        }
#ifdef _WIN32
        const auto fname = path.filename().native();
        if (fname.find(static_cast<native_char>(':'))
                != std::filesystem::path::string_type::npos) {
            throw PdfeException(
                "Refusing to save: Windows alternate data stream "
                "selector ':' in filename");
        }

        // GitHub #156 / #95 — THE LONG-PATH CONTRACT IS *OFF*, AND THIS IS
        // WHERE IT IS ENFORCED.
        //
        // PDFE declares no support for save targets beyond MAX_PATH. The
        // decision (recorded on #95) is that an "on" contract is untestable:
        // long-path support needs BOTH a `longPathAware` application manifest
        // — this repo ships none — AND the machine-wide `LongPathsEnabled`
        // registry policy, so it would be true on some machines and false on
        // others. A guarantee that varies by machine is not a guarantee.
        //
        // "Off" therefore has to mean something stronger than "it happens to
        // fail somewhere downstream". Downstream failure is exactly what the
        // old behaviour was, and it was neither clean nor stable: some
        // internals reach for the `\\?\` extended-length form above 240
        // characters (AtomicFileReplace::win32Path, added by F4 so that a
        // 251-259 character destination survives the 9-character staging
        // suffix), so whether a 300-character save succeeded depended on
        // which code path happened to touch the name. Reject it HERE, before
        // anything touches the filesystem, so the outcome is one thing on
        // every machine and NO partial file can be left at the target.
        //
        // The bound is the usable Win32 limit: MAX_PATH counts the trailing
        // NUL, so 259 characters is the longest accepted path. F4's 251-259
        // window is deliberately still inside it.
        // ...WITH ONE EXEMPTION, and it is not a loophole: a caller that has
        // ALREADY written the `\\?\` extended-length prefix itself.
        //
        // #95 decided the MANIFEST contract. The prefix is a different
        // mechanism and the decision above does not reach it: `\\?\` needs
        // neither the `longPathAware` manifest nor the `LongPathsEnabled`
        // policy, because it bypasses the Win32 path parser outright. It
        // therefore behaves IDENTICALLY on every Windows machine — which is
        // precisely the property the paragraph above demands, so refusing it
        // would contradict the reasoning rather than implement it.
        //
        // This is also not hypothetical: CliTest.OpenFile_RealLongPathBeyond-
        // MaxPath_OpensAndSaves has been round-tripping a genuine 300+ char
        // `\\?\` target for as long as it has existed. Rejecting it here was
        // an over-application of #95 that silently withdrew a working,
        // tested capability. An unprefixed path of the same length is still
        // refused, which is what LongPathIsRejectedWithALengthNamingError
        // pins.
        constexpr std::size_t kMaxWin32PathChars = 259;
        const bool isExtendedLengthPath =
            native_path.rfind(L"\\\\?\\", 0) == 0;
        if (!isExtendedLengthPath && native_path.size() > kMaxWin32PathChars) {
            throw PdfeException(
                "Refusing to save: path length "
                + std::to_string(native_path.size())
                + " characters exceeds the Windows maximum path length of "
                + std::to_string(kMaxWin32PathChars)
                + " characters (PDFE does not support long paths); "
                  "choose a shorter name or a shallower folder");
        }
#endif
    }

    // F24 — certification guard, same placement and reasoning as in save().
    runSaveGuard(path);

    // GitHub #19 / PDF-50 — signed-rewrite guard, same placement as in save().
    runSignedRewriteGuard(path);

    pImpl->fireEvent("willSave");

    // No page-tree flush here: pdf_drop_page_tree() is a documented no-op
    // in MuPDF 1.26.10 (pdf-page.c:152) and MuPDF invalidates the page map
    // itself. See docs/audit/page-tree-flush-noop-2026-08-31.md.

    // Clear dirty page set since we're about to save everything
    {
        std::lock_guard<std::mutex> lock(pImpl->docMutex);
        pImpl->dirtyPageIndices_.clear();
    }

    // F10 / WIN-76 (Critical, data-loss): "Save As" ONTO AN EXISTING FILE used to
    // hand `path` straight to pdf_save_document, which fopen("wb")s it — i.e. it
    // TRUNCATED the user's existing file and only then started writing. Any write
    // failure from that point on (disk full, volume detached, media removed, the
    // process killed) left the victim file truncated or half-written: the target's
    // previous contents were destroyed even though the save never completed.
    //
    // Route an overwrite through the SAME temp-write → atomic-replace window
    // save() uses, so the destination is byte-intact until the instant it is
    // replaced and is COMPLETELY UNTOUCHED on any failure.
    //
    // Two cases deliberately keep the direct write:
    //   * the destination EXISTS and is not a regular file (a directory, a
    //     device) — the direct write's immediate, precise error is better than
    //     writing a staging file we would then fail to rename;
    //   * the destination IS the file we currently have open — MuPDF holds a read
    //     handle on it, so an atomic replace could never delete it on Windows;
    //     that case keeps its long-standing in-place behaviour.
    //
    // WIN-87 (phase-56) — WHAT CHANGED HERE, AND THE ONE CASE THAT MOVED.
    // The staging file is now created behind an exclusive handle and published
    // through it, exactly as in save(); the write, the durability flush and the
    // rename all use that handle, so the staging name is never re-resolved. The
    // "best-effort stale-temp clear" that used to be a bare
    // std::filesystem::remove() is gone: it followed a reparse point planted at
    // the temp name, and it raced whoever planted it. That job now belongs to
    // StagingFile::createFullRewriteTempFor, which classifies by attribute,
    // unlinks the LINK rather than its target, refuses a real directory, and
    // retries the clear-then-CREATE_NEW pair so a re-planter loses.
    //
    // The DESTINATION-DOES-NOT-EXIST case also moved onto staging, where it
    // used to write straight to `path`. It is listed in criterion 1 alongside
    // the overwrite, and it costs nothing: the rename simply has no file to
    // replace. It buys the same all-or-nothing property the overwrite case has
    // — a failed Save As to a new name now leaves NO half-written file at that
    // name, instead of a truncated one the user may later open as a document.
    // GitHub #100 / WIN-78 — free-space pre-check for the Save As target. Note
    // the target here is usually a DIFFERENT VOLUME from the source, which is
    // precisely why the check reads space for `path` and not for filePath.
    // Advisory only; see Document::save for the reasoning.
    (void)pdfe::core::warnIfInsufficientSpaceForSave(path, pImpl->filePath);

    std::error_code existEc;
    const bool destinationExists = std::filesystem::exists(path, existEc);
    std::error_code regEc;
    const bool destinationIsRegularFile =
        std::filesystem::is_regular_file(path, regEc) && !regEc;
    const bool destinationIsCurrentlyOpenFile =
        !pImpl->filePath.empty() && path == pImpl->filePath;
    const bool staged =
        !destinationIsCurrentlyOpenFile
        && (!destinationExists || destinationIsRegularFile);

    std::unique_ptr<StagingFile> staging;
    if (staged) {
        std::error_code stageEc;
        staging = StagingFile::createFullRewriteTempFor(path, stageEc);
        if (!staging) {
            throw PdfeException(
                "Could not create the staging file for " + path.string()
                + " (target left intact): " + stageEc.message());
        }
    }

    // Tier 2 (pipeline b2e47d91): gc=3 only when a dedup-worthy op occurred;
    // otherwise the cheap default — a "Save As" of a large XFA form with no
    // structural edit now finishes in ~gc=1 time instead of minutes.
    const int gcLevel = effectiveGarbageLevel();
    // F14 (PDF-01): never emit a header EARLIER than the document's version.
    applyVersionFloorForSave();
    {
        pdf_write_options opts{};
        opts.do_garbage = gcLevel;
        opts.do_compress = 1;
        // Safe per the same MuPDF /Filter guard documented at save() above
        // (pipeline 3f8a1c72 Phase 1): already-compressed images/fonts are copied
        // verbatim; only filterless streams are Flate-compressed. Not lossy.
        opts.do_compress_images = 1;
        opts.do_compress_fonts = 1;
        opts.do_preserve_metadata = 1;
        // PDF-02: keep the compact on-disk shape the document arrived with.
        // Deliberately NOT applied to the two *ThroughExternalRewrite bakes:
        // those are intermediates handed to a converter that decides the final
        // shape itself, and PDF/A-1 forbids object streams outright — imposing
        // them on a bake would be arguing with the converter about its own
        // output format.
        opts.do_use_objstms = effectiveObjectStreamMode();
        if (staged) {
            // On a throw the staging file is removed by `staging`'s destructor
            // and the DESTINATION was never opened, so it is byte-identical —
            // the same contract the previous try/catch + remove() provided,
            // with no path left for it to miss.
            writeFullRewriteThroughStagingHandle(*pImpl->context, pImpl->pdfDoc,
                                                 *staging, opts);
        } else {
            std::string pathStr = mupdfPathUtf8(path);
            pImpl->context->execute([this, &pathStr, &opts](fz_context* ctx) {
                pdf_save_document(ctx, pImpl->pdfDoc, pathStr.c_str(), &opts);
            });
        }
    }

    if (staged) {
        // Durability through the exclusive handle (never a reopen by name),
        // then the atomic swap with the anti-A/V retry (WIN-84).
        {
            std::error_code flushEc;
            staging->flushToStableStorage(flushEc);  // best-effort
        }
        std::error_code ec;
        if (!staging->commitOverWithRetry(path, ec)) {
            // `staging`'s destructor removes the orphan as this throw unwinds.
            throw PdfeException(
                "Failed to atomically replace target file (target left "
                "intact): " + ec.message());
        }
    }

    // GitHub #107 (WIN-96): propagate the source file's Mark-of-the-Web.
    //
    // MUST run BEFORE pImpl->filePath is reassigned below -- at this point it
    // still names the file we opened, which is the file whose provenance we
    // are preserving. After the assignment the source is unrecoverable.
    //
    // Windows records provenance in a Zone.Identifier alternate data stream;
    // ZoneId=3 means "from the internet", and SmartScreen, Office Protected
    // View and the shell all act on it. Without this, opening a downloaded PDF
    // and saving it elsewhere produced a copy the platform believed was
    // locally authored -- a silent provenance downgrade performed by the
    // editor, on the user's behalf, without telling them.
    //
    // PROPAGATION, not marking: a locally-authored PDF has no mark and must
    // not acquire one, or everything would land in Protected View and the mark
    // would stop meaning anything.
    //
    // Best-effort and deliberately non-throwing: the save has already
    // succeeded, alternate data streams do not exist on FAT32 or most network
    // shares, and failing to copy a mark is not a reason to fail the user's
    // save. The outcome is observable via propagateZoneIdentifier's return for
    // tests; it is not surfaced to the user because there is no action they
    // could take.
    if (!pImpl->filePath.empty() && pImpl->filePath != path) {
        (void)pdfe::core::propagateZoneIdentifier(pImpl->filePath, path);
    }

    pImpl->filePath = path;
    pImpl->modified = false;
    // saveAs full-rewrites (scrubbing orphans → clear the taint) but, unlike
    // save(), does NOT reopen the written file. do_garbage renumbered the
    // on-disk objects, so the in-memory numbering no longer matches `path`;
    // an incremental append would corrupt it. Mark incremental-unsafe until
    // the next full-rewrite save() reopens and resyncs.
    pImpl->requiresFullRewrite_ = false;
    pImpl->needsFullGc_ = false;  // the full rewrite (with dedup if needed) just ran
    pImpl->incrementalSafe_ = false;
    pImpl->fireEvent("didSave");
}

void Document::saveIncremental() {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document — cannot save");
    }
    if (pImpl->filePath.empty()) {
        throw PdfeException(
            "Incremental save requires an originating file path");
    }

    // F24 — certification guard. An incremental append ALWAYS targets the file
    // the document was opened from, so this is always the in-place case: the
    // one the guard refuses for a broken certification.
    runSaveGuard(pImpl->filePath);

    // No page-tree flush here: pdf_drop_page_tree() is a documented no-op
    // in MuPDF 1.26.10 (pdf-page.c:152) and MuPDF invalidates the page map
    // itself. See docs/audit/page-tree-flush-noop-2026-08-31.md.
    {
        std::lock_guard<std::mutex> lock(pImpl->docMutex);
        pImpl->dirtyPageIndices_.clear();
    }

    const std::string pathStr = mupdfPathUtf8(pImpl->filePath);
    pImpl->context->execute([this, &pathStr](fz_context* ctx) {
        pdf_write_options opts{};
        opts.do_incremental = 1;
        opts.do_preserve_metadata = 1;
        // Incremental save must NOT garbage-collect, compress, or
        // rewrite existing streams — the whole point is preserving
        // the original byte prefix so /ByteRange stays valid.
        pdf_save_document(ctx, pImpl->pdfDoc, pathStr.c_str(), &opts);
    });

    pImpl->modified = false;
}

// ---------------------------------------------------------------------------
// F58 (phase-52) — the staged, crash-safe incremental save.
// F58-a/-b/-f (phase-53) — and the staging file made harder to attack than the
// document it protects.
//
// See Document.h for the contract. The sequence is the SAME shape as save()'s
// full-rewrite path (stage → flush → drop handle → atomic replace → reopen),
// differing only in step 1: an incremental revision has to be appended to the
// ORIGINAL BYTES, so the staging file starts life as a byte copy of the target
// rather than as an empty file. That is what keeps the original a byte-identical
// prefix of the result, which is the whole reason an incremental save exists — a
// full rewrite renumbers and re-serialises and would invalidate every existing
// signature's /ByteRange.
//
// WHAT PHASE-52 GOT WRONG, AND HOW (F58-a, CRITICAL — EXPLOITED, NOT THEORISED).
// The staging file was created by std::filesystem::copy_file (CopyFileW: no
// CREATE_NEW, no restrictive share mode) at the fully predictable path
// "<target>.pdfe_tmp", and NO handle was held on it between the append finishing
// and the rename. An unprivileged process polled for that path and stamped 64
// bytes over offset 0 — a region MuPDF had already passed, because an
// incremental append seeks to EOF. PDFE then atomically published the attacker's
// bytes over the user's signed document and reported SUCCESS; the published file
// was not even a PDF. It is decisive precisely where the old in-place append was
// safe: the staging file is NEW, so it is writable when the read-only or
// tightly-ACL'd original is not, and unlocked when the original is held open.
//
// THE ROUTE TAKEN. Not the fallback — the full fix:
//   * core::StagingFile creates "<target>.pdfe_part.<128-bit CSPRNG hex>" with
//     CREATE_NEW and holds GENERIC_READ|GENERIC_WRITE|DELETE with dwShareMode =
//     FILE_SHARE_READ for the ENTIRE window;
//   * the original is copied in THROUGH that handle (never CopyFileW);
//   * MuPDF is handed a DUPLICATE of that handle as a stdio stream
//     (_open_osfhandle/_fdopen → fz_new_output_with_file_ptr →
//     pdf_write_document), so it never reopens the staging file by name;
//   * publication is SetFileInformationByHandle(FileRenameInfoEx,
//     POSIX_SEMANTICS|REPLACE_IF_EXISTS) on that same handle.
// There is therefore no instant, from creation to publication, at which the
// staged bytes are openable for writing or deletion by anyone else.
//
// READ THAT AS INTEGRITY, BECAUSE THAT IS ALL IT CLAIMS (F71). "Writing or
// deletion" is not "reading", and the omission was load-bearing: until F71 the
// staging file carried the CONTAINING DIRECTORY's inherited ACEs for the whole
// append, so a restrictively-ACL'd document had a full plaintext copy readable
// by every principal that directory admits, for as long as the save took. The
// sentence above was true throughout. It just never covered that.
// StagingFile now applies the destination's DACL at creation, which closes it.
//
// THE CASE THAT USED NOT TO TAKE IT, AND NOW DOES (F62, phase-55 — READ THIS
// BEFORE REINTRODUCING A BY-PATH WRITE HERE). pdf_save_document sets
// `opts.bias = doc->bias` before writing incrementally; pdf_write_document does
// not (an upstream asymmetry, verified in MuPDF 1.26.10 pdf-write.c). `bias` is
// non-zero only for a document with garbage before its `%PDF-` marker — rare,
// but reachable, because PDFE accepts a header preamble. Phases 52–53 concluded
// from that asymmetry that bias != 0 had to go through pdf_save_document BY
// PATH, releasing the exclusive handle for the duration and re-verifying the
// staged PREFIX afterwards.
//
// That fallback was wrong, and F62 is the finding. The re-verification covered
// `[0, originalSize)` and never `[originalSize, end)` — the region the by-path
// write had just produced and the region actually exposed. A forged, LARGER
// tail passed the prefix check and the must-not-shrink check and would have been
// published over the user's document. It could not be fixed by hashing the tail
// either: hashing "as written" needs an output-side tee, and a by-path write has
// no fz_output to wrap.
//
// So the window was REMOVED rather than verified. Every document, biased or not,
// is now appended to through the exclusive handle, using a bias-compensating
// fz_output that applies the same correction MuPDF's private `opts.bias` applies
// — see the BiasedStagingOutput block comment above for the mechanism, the
// boundary it pivots on, the fail-closed checks that pin it, and the one
// combination (bias != 0 plus a signature being created in the same save) that
// is refused instead. StagingFile::releaseHandleForExternalWrite and
// ::reacquireExclusiveHandle are deleted, so the window cannot be reintroduced
// by reaching for them.
//
// WHY THE MuPDF READ HANDLE ON THE TARGET MUST STILL BE DROPPED. Measured, not
// assumed: with a CRT _wfopen(..., "rb") handle open on the destination (which is
// exactly how MuPDF's fz_open_file opens it), a replace-existing rename onto it
// fails with ERROR_ACCESS_DENIED (5). Document::save() has always dropped the
// document before its rename for this reason. (FileRenameInfoEx's POSIX semantics
// relax this for OTHER processes' handles, not for a handle in this process that
// the same rename must supersede.)
// ---------------------------------------------------------------------------
void Document::saveIncrementalStaged(bool* reloadedFromDisk,
                                     bool* publishedButReopenFailed) {
    if (reloadedFromDisk) *reloadedFromDisk = false;
    if (publishedButReopenFailed) *publishedButReopenFailed = false;

    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document — cannot save");
    }
    if (pImpl->filePath.empty()) {
        throw PdfeException(
            "Incremental save requires an originating file path");
    }

    const std::filesystem::path target = pImpl->filePath;

    // F24 — certification guard, same placement and reasoning as in
    // saveIncremental(): an incremental append always targets the file the
    // document was opened from, so this is always the in-place case.
    runSaveGuard(target);

    std::error_code sizeEc;
    const std::uintmax_t originalSize = std::filesystem::file_size(target, sizeEc);
    if (sizeEc || originalSize == 0) {
        throw PdfeException(
            "Cannot size the document being appended to (original left "
            "intact): " + sizeEc.message());
    }

    // 1. STAGE. Same directory as the target, so same volume, so the rename
    //    really is a single atomic directory operation. The unique_ptr is the
    //    replacement for phase-52's TempFileGuard and is strictly stronger: its
    //    destructor closes the exclusive handle AND removes the staging file on
    //    every throwing path below, and it cannot be defeated by a read-only
    //    attribute because the staging file never inherits one (it is created
    //    fresh, not CopyFileW'd).
    std::error_code ec;
    std::unique_ptr<StagingFile> staging = StagingFile::createBeside(target, ec);
    if (!staging) {
        throw PdfeException(
            "Could not create the staging file beside " + target.string()
            + " (original left intact): " + ec.message());
    }
    if (!staging->copyFrom(target, ec)) {
        throw PdfeException(
            "Could not stage the incremental save (original left intact): "
            + ec.message());
    }

    // 2. No page-tree flush here: pdf_drop_page_tree() is a documented no-op
    // in MuPDF 1.26.10 (pdf-page.c:152) and MuPDF invalidates the page map
    // itself. See docs/audit/page-tree-flush-noop-2026-08-31.md.
    {
        std::lock_guard<std::mutex> lock(pImpl->docMutex);
        pImpl->dirtyPageIndices_.clear();
    }

    // 3. APPEND onto the staged copy. The recorded xref offsets are offsets into
    //    the original bytes, which the staged copy reproduces exactly, so
    //    appending at its EOF is byte-for-byte what appending to the original
    //    would have produced.
    //
    //    F62 (phase-55): ONE route, for every document. See the
    //    BiasedStagingOutput block comment above for why the bias != 0 case no
    //    longer needs — and no longer has — a by-path fallback that lets go of
    //    the exclusive handle.
    const int docBias = pImpl->context->executeReturn<int>(
        [this](fz_context*) -> int { return pImpl->pdfDoc->bias; });

    if (docBias != 0) {
        // The one combination no external output can serve; refused rather than
        // written wrong. See the block comment: MuPDF emits a NEW signature's
        // /ByteRange in physical coordinates and the xref in relative ones, and
        // only the private opts.bias can produce both.
        const bool unsavedSigs = pImpl->context->executeReturn<bool>(
            [this](fz_context* ctx) -> bool {
                return pdf_has_unsaved_sigs(ctx, pImpl->pdfDoc) != 0;
            });
        if (unsavedSigs) {
            throw PdfeException(
                "This document has garbage before its %PDF- marker AND a "
                "signature waiting to be written in the same save. PDFE cannot "
                "produce a correct /ByteRange for that combination, and will "
                "not publish one it cannot vouch for (original left intact).");
        }
    }

    // F-1 (phase-55 security review). prefixLength is compared against the
    // position MuPDF truncates at, and that position is `doc->file_size`
    // (pdf-write.c:2384 passes it straight to
    // ensure_initial_incremental_contents). In MuPDF 1.26.10 that field has
    // exactly three assignments in source/pdf: pdf-xref.c:971 at load,
    // pdf-xref.c:3661 zeroing it at document creation, and pdf-object.c:1750
    // in the JOURNAL/SNAPSHOT RESTORE path. None of them runs after an
    // incremental save. It is therefore MuPDF's view of the document, not the
    // disk's — and because the journal path CAN move it mid-session, it must be
    // read HERE, at save time, rather than cached earlier: a snapshot restore
    // between an earlier read and this one would reintroduce exactly the
    // mismatch this fixes.
    // Using std::filesystem::file_size(target) here compared the wrong two
    // quantities: it asserted "the file on disk has not changed size since we
    // opened it", so a document another writer appended to while it was open
    // produced a refusal blaming MuPDF drift, sending a maintainer to read
    // pdf-write.c for a defect that is not there — and LtvDssWriter.cpp
    // documents that grew-on-disk case as HANDLED, not as an error.
    // `originalSize` is still the right quantity for the must-not-shrink check
    // below, which really is about the bytes on disk.
    const std::int64_t muDocSize = pImpl->context->executeReturn<std::int64_t>(
        [this](fz_context*) -> std::int64_t {
            return static_cast<std::int64_t>(pImpl->pdfDoc->file_size);
        });

    BiasedStagingOutput outState;
    outState.bias = static_cast<std::int64_t>(docBias);
    outState.prefixLength = muDocSize;
    outState.fp = staging->openDuplicateStream(ec);
    if (!outState.fp) {
        throw PdfeException(
            "Could not open the staging stream (original left intact): "
            + ec.message());
    }
#if defined(_WIN32)
    // Match fz_new_output_with_file_ptr: MuPDF does its own buffering, and an
    // unbuffered FILE* keeps ftell/_chsize_s agreeing with what has been
    // written without depending on a flush we did not make.
    ::setvbuf(outState.fp, nullptr, _IONBF, 0);
#else
    ::setvbuf(outState.fp, nullptr, _IONBF, 0);
#endif

    // Ownership of `outState.fp` passes to MuPDF here: biasedOutDrop fcloses it
    // and fz_drop_output calls that — on the throwing path too, including the
    // case where fz_new_output itself fails (it calls the drop function before
    // rethrowing). Nothing below may fclose it again. `outState` itself is ours
    // and outlives the output, because its truncate bookkeeping is what the
    // publish decision below turns on. The DUPLICATE handle dying costs us
    // nothing: StagingFile's own exclusive handle is untouched and still spans
    // the whole window, from creation to publication.
    pImpl->context->execute([this, &outState](fz_context* ctx) {
        fz_output* out = NULL;
        fz_var(out);
        // No C++ objects with destructors are constructed inside this
        // fz_try region — MuPDF's setjmp/longjmp would not unwind them.
        fz_try(ctx) {
            out = fz_new_output(ctx, 8192, &outState, biasedOutWrite, NULL,
                                biasedOutDrop);
            out->seek = biasedOutSeek;
            out->tell = biasedOutTell;
            out->as_stream = biasedOutAsStream;
            out->truncate = biasedOutTruncate;
            fz_seek_output(ctx, out, 0, SEEK_END);
            pdf_write_options opts;
            memset(&opts, 0, sizeof(opts));
            opts.do_incremental = 1;
            opts.do_preserve_metadata = 1;
            // No GC, no compression: the original byte prefix must survive
            // untouched or every existing signature's /ByteRange breaks.
            pdf_write_document(ctx, pImpl->pdfDoc, out, &opts);
            fz_close_output(ctx, out);
        }
        fz_always(ctx) {
            fz_drop_output(ctx, out);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });

    // The bias compensation is only correct if the boundary landed where the
    // whole scheme assumes. Checked, not assumed — and checked AFTER the write
    // so a MuPDF whose call pattern changed refuses to publish rather than
    // publishing offsets that are silently wrong by `bias`.
    if (docBias != 0 && (outState.truncateCalls != 1 || !outState.biasActive)) {
        throw PdfeException(
            "The staged incremental append did not reach the point at which "
            "this build applies the header-preamble offset correction, so its "
            "xref offsets cannot be vouched for — refusing to publish "
            "(original left intact).");
    }

    // NO PREFIX RE-VERIFICATION HERE, AND THAT IS THE POINT (F62, phase-55).
    // The deleted fallback needed one because it had released the exclusive
    // handle and had to ask, afterwards, whether the bytes that came back were
    // still the bytes it left. Nothing releases the handle any more, so there is
    // no interval in which the staged prefix could change and nothing to
    // re-establish. Re-reading it here would ALSO be a regression rather than
    // insurance: it would have to read the ORIGINAL to compare against, and the
    // original is not always readable at this instant — F58-f's own regression
    // test deliberately denies this user READ on the target mid-append to prove
    // a published-but-unreopenable revision is reported honestly. A check that
    // turns that scenario into a refused save would be verifying a property
    // nothing can violate at the cost of one that is exercised.

    // 4. Sanity: the staged copy must never have SHRUNK. Growth is the normal
    //    case but is NOT required — a save of a document with no unsaved object
    //    changes legitimately appends nothing, and publishing a byte-identical
    //    file is a correct no-op (SaveOperationsTest.SaveCreatesFile is exactly
    //    that shape). A file that got SHORTER, by contrast, means the original
    //    bytes are no longer a prefix of it, i.e. every existing /ByteRange in
    //    the document is now wrong — the one outcome staging exists to prevent.
    {
        std::uintmax_t stagedSize = 0;
        if (!staging->sizeInBytes(stagedSize, ec) || stagedSize < originalSize) {
            throw PdfeException(
                "The staged incremental save SHRANK the document — the original "
                "bytes are no longer a prefix (original left intact)");
        }
    }

    // 5. Push the staged bytes toward stable storage before they are published.
    //    Through the exclusive handle — no reopen-by-path, which would be one
    //    more nameable moment.
    {
        std::error_code flushEc;
        staging->flushToStableStorage(flushEc);  // best-effort, as in save()
    }

    // 6. Release MuPDF's read handle on the target — see the header comment.
    pImpl->context->execute([this](fz_context* ctx) {
        fz_drop_document(ctx, pImpl->doc);
    });
    pImpl->doc = nullptr;
    pImpl->pdfDoc = nullptr;
    // F58-f: the cached count described a document that no longer exists. Left
    // stale, a reopen failure below would leave pageCount() reporting a non-zero
    // count for a null fz_document and page(i) range-checking against it before
    // calling fz_load_page(ctx, nullptr, i) — a null dereference, reachable from
    // drainRebindAroundWrite, which does exactly pageCount() then page(idx).
    pImpl->cachedPageCount = 0;
    // Set BEFORE the replace is attempted: from this point on the in-memory
    // mutation is gone whichever way the replace goes, and a caller holding a
    // rollback snapshot must not try to apply it to the re-opened document.
    if (reloadedFromDisk) *reloadedFromDisk = true;

    // 7. COMMIT, through the exclusive handle. Never removes the destination
    //    first, so on any failure the original is still byte-intact on disk —
    //    and the destination's DACL is carried onto the staged file first, so
    //    publication can never WIDEN the document's permissions (F58-b).
    const bool replaced = staging->commitOver(target, ec);

    // 8. Reopen, on BOTH paths: on success from the newly published file, on
    //    failure from the untouched original. Either way the caller is left
    //    holding a live Document rather than a dead null handle — the same
    //    contract save()'s failure branch keeps.
    const std::string pathStr = mupdfPathUtf8(target);
    try {
        pImpl->doc = pImpl->context->executeReturn<fz_document*>(
            [&pathStr](fz_context* ctx) -> fz_document* {
                return fz_open_document(ctx, pathStr.c_str());
            }
        );
        pImpl->pdfDoc = pImpl->context->executeReturn<pdf_document*>(
            [this](fz_context* ctx) -> pdf_document* {
                return pdf_specifics(ctx, pImpl->doc);
            }
        );
        pImpl->cachedPageCount = pImpl->context->executeReturn<int>(
            [this](fz_context* ctx) -> int {
                return fz_count_pages(ctx, pImpl->doc);
            }
        );
    } catch (const std::exception& reopenFailure) {
        // F58-f. An A/V scanner or the search indexer holding a sharing
        // violation on the just-renamed file is routine on Windows. Before this
        // branch existed the exception simply propagated, leaving a zombie
        // Document (null handle, stale page count) AND — worse — telling the
        // user the /DSS upgrade FAILED when the revision had in fact been
        // published. Those two outcomes are now distinguishable.
        pImpl->doc = nullptr;
        pImpl->pdfDoc = nullptr;
        pImpl->cachedPageCount = 0;
        if (replaced) {
            pImpl->modified = false;
            pImpl->incrementalSafe_ = false;  // no reopen ⇒ numbering unverified
            if (publishedButReopenFailed) *publishedButReopenFailed = true;
            throw PdfeException(
                std::string("The incremental save WAS published successfully — "
                            "the document on disk is correct and complete — but "
                            "it could not be re-opened afterwards (another "
                            "process is holding it): ")
                + reopenFailure.what());
        }
        throw PdfeException(
            std::string(kAtomicPublishFailedPrefix)
            + " (original left intact), and the original could not be "
              "re-opened either: "
            + reopenFailure.what());
    }

    if (!replaced) {
        // `staging`'s destructor removes the orphan as this throw unwinds.
        throw PdfeException(
            std::string(kAtomicPublishFailedPrefix)
            + " (original left intact): " + ec.message());
    }

    pImpl->modified = false;
    // The reopen re-synced the in-memory numbering to `target`, which is
    // exactly filePath(), so the next append is safe.
    pImpl->incrementalSafe_ = true;
}

void Document::writeRecoveryCopy(const std::filesystem::path& path) const {
    if (!pImpl->pdfDoc) {
        throw PdfeException("Not a PDF document — cannot write recovery copy");
    }

    // F24 — deliberately NOT guarded by runSaveGuard(). A recovery sidecar is
    // written to a DISTINCT path, is never the certified original, and exists
    // precisely so a crash cannot lose the user's work; refusing it would turn
    // a certification warning into data loss. The guard fires when that work is
    // saved for real.
    //
    // Deliberately do NOT fire willSave/didSave here: an autosave is an
    // out-of-band background snapshot, not a user-visible save; running
    // document-level JS "willSave"/"didSave" hooks on every 30s tick would be
    // surprising (and could mutate the live document, e.g. a "last saved" form
    // field), corrupting the very state we are trying to preserve verbatim.

    // No page-tree flush here: pdf_drop_page_tree() is a documented no-op
    // in MuPDF 1.26.10 (pdf-page.c:152) and MuPDF invalidates the page map
    // itself. See docs/audit/page-tree-flush-noop-2026-08-31.md.

    const std::string pathStr = mupdfPathUtf8(path);
    // F14 (PDF-01): a recovery snapshot must round-trip at the document's own
    // version too — recovering an edit must not silently downgrade the file.
    applyVersionFloorForSave();
    pImpl->context->execute([this, &pathStr](fz_context* ctx) {
        pdf_write_options opts{};
        // A recovery snapshot must be FAST, not small. It is a transient file
        // written on a background timer (potentially every interval while the
        // user keeps editing) and deleted on the next clean save, so paying to
        // shrink it is wasted work — and the cost that matters is re-encoding.
        //   * do_garbage      = 0 : skip the object-graph sweep.
        //   * do_compress*    = 0 : DO NOT re-flate / re-encode existing image
        //                           and font streams. On an image-heavy PDF the
        //                           image recompression pass dominates wall time
        //                           (seconds on a 25 MB scan); preserving the
        //                           already-encoded streams verbatim makes the
        //                           write essentially a serialize-and-copy.
        // The resulting file is larger than a real File>Save but just as valid
        // and openable, which is all crash-recovery needs. A real save (with
        // full GC/compression) still runs when the user explicitly saves.
        opts.do_garbage = 0;
        opts.do_compress = 0;
        opts.do_compress_images = 0;
        opts.do_compress_fonts = 0;
        opts.do_preserve_metadata = 1;
        pdf_save_document(ctx, pImpl->pdfDoc, pathStr.c_str(), &opts);
    });

    // Flush the recovery bytes toward stable storage so a crash immediately
    // after the autosave still leaves a complete, openable recovery file.
    flushFileToStableStorage(path);

    // CRITICAL: touch NONE of the session save-routing state. filePath,
    // requiresFullRewrite_, needsFullGc_, incrementalSafe_, modified, and the
    // dirty-page set are all left exactly as they were, so the next real
    // File>Save routes identically to how it would have without this autosave.
}

std::optional<std::filesystem::path> Document::pendingRecoveredTemp() const {
    return pImpl->pendingRecoveredTemp_;
}

bool Document::pendingRecoveredTempIsComplete() const {
    return pImpl->pendingRecoveredTempComplete_;
}

void Document::refreshPageCount() {
    pImpl->cachedPageCount = pImpl->context->executeReturn<int>(
        [this](fz_context* ctx) -> int {
            return fz_count_pages(ctx, pImpl->doc);
        }
    );
}

void Document::setDocumentEventCallback(DocumentEventCallback cb) {
    pImpl->eventCb_ = std::move(cb);
}

} // namespace pdfe::core
