#include "HeaderFooterEngine.h"
#include "../core/MuPdfContext.h"
#include "core/FormXObjectDepthGuard.h"
#include "../core/PathSecurity.h"
#include "../core/ContentStreamPreamble.h"
#include "../core/PageContentGuard.h"
#include "../signatures/DocMdpDirectWriteGuard.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pdfe::headerfooter {

struct HeaderFooterEngine::Impl {
    std::shared_ptr<pdfe::core::MuPdfContext> ctx;
};

HeaderFooterEngine::HeaderFooterEngine(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>())
{
    impl_->ctx = std::move(context);
    if (!impl_->ctx) {
        throw std::invalid_argument("HeaderFooterEngine: context must not be null");
    }
}

HeaderFooterEngine::~HeaderFooterEngine() = default;

// ---------- Token resolution ----------

namespace {

std::string replaceAll(std::string s, const std::string& needle, const std::string& repl)
{
    if (needle.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        s.replace(pos, needle.size(), repl);
        pos += repl.size();
    }
    return s;
}

// Escape for PDF literal string: () and \\ need escaping.
std::string escapePdfLiteral(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '(' || c == ')' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string isoDateToday()
{
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return buf;
}

} // namespace

namespace {

// Expand a single {bates:PREFIX:START:WIDTH} or {bates:PREFIX:START}
// (WIDTH defaults to 6) token against a 0-based page index. Returns
// empty string when the token is malformed so the caller can leave
// the original text alone.
std::string expandBatesToken(const std::string& body,
                             int pageIndexZeroBased)
{
    // body looks like "PREFIX:START[:WIDTH]"
    auto firstColon = body.find(':');
    if (firstColon == std::string::npos) return {};
    const std::string prefix = body.substr(0, firstColon);
    const std::string rest = body.substr(firstColon + 1);
    auto secondColon = rest.find(':');
    std::string startStr, widthStr;
    if (secondColon == std::string::npos) {
        startStr = rest;
        widthStr = "6";
    } else {
        startStr = rest.substr(0, secondColon);
        widthStr = rest.substr(secondColon + 1);
    }
    long long start = 0;
    int width = 6;
    try {
        start = std::stoll(startStr);
        width = std::stoi(widthStr);
    } catch (...) { return {}; }
    if (width < 1 || width > 20) width = 6;

    const long long n = start + pageIndexZeroBased;
    std::string digits = std::to_string(n);
    if ((int)digits.size() < width) {
        digits.insert(0, width - digits.size(), '0');
    }
    return prefix + digits;
}

// Walk `s` expanding every {bates:...} occurrence. Done as a manual
// scan because replaceAll operates on fixed needles.
std::string expandAllBatesTokens(const std::string& s, int pageIndexZeroBased)
{
    std::string out;
    out.reserve(s.size());
    const std::string kPrefix = "{bates:";
    size_t pos = 0;
    while (pos < s.size()) {
        size_t start = s.find(kPrefix, pos);
        if (start == std::string::npos) {
            out.append(s, pos, std::string::npos);
            break;
        }
        out.append(s, pos, start - pos);
        size_t end = s.find('}', start + kPrefix.size());
        if (end == std::string::npos) {
            out.append(s, start, std::string::npos);
            break;
        }
        std::string body = s.substr(start + kPrefix.size(),
                                    end - start - kPrefix.size());
        std::string expanded = expandBatesToken(body, pageIndexZeroBased);
        if (expanded.empty()) {
            // Leave the original token in place on parse failure.
            out.append(s, start, end - start + 1);
        } else {
            out.append(expanded);
        }
        pos = end + 1;
    }
    return out;
}

} // namespace

std::string HeaderFooterEngine::resolveTokens(
    const std::string& raw,
    int pageIndexZeroBased,
    int totalPages,
    const std::string& author,
    const std::string& filename,
    const std::string& isoDate)
{
    std::string s = raw;
    s = replaceAll(s, "{date}",     isoDate);
    s = replaceAll(s, "{author}",   author);
    s = replaceAll(s, "{page}",     std::to_string(pageIndexZeroBased + 1));
    s = replaceAll(s, "{pages}",    std::to_string(totalPages));
    s = replaceAll(s, "{filename}", filename);
    // Bates stamping: {bates:PREFIX:START:WIDTH}. WIDTH optional (default 6).
    s = expandAllBatesTokens(s, pageIndexZeroBased);
    return s;
}

bool HeaderFooterEngine::pageInRange(int pageIndexZeroBased,
                                     int totalPages,
                                     const HeaderFooterConfig& cfg)
{
    if (pageIndexZeroBased < 0 || pageIndexZeroBased >= totalPages) return false;
    const int oneBased = pageIndexZeroBased + 1;
    switch (cfg.rangeMode) {
        case PageRangeMode::All:           return true;
        case PageRangeMode::OddPagesOnly:  return (oneBased % 2) == 1;
        case PageRangeMode::EvenPagesOnly: return (oneBased % 2) == 0;
        case PageRangeMode::Explicit:
            return std::find(cfg.explicitPages.begin(),
                             cfg.explicitPages.end(),
                             pageIndexZeroBased) != cfg.explicitPages.end();
    }
    return false;
}

// ---------- Content-stream assembly ----------

namespace {

// Build one tagged content stream covering up to 6 slots for this page.
// Empty slots are omitted; if all slots resolve empty the returned string
// is empty and the caller should not attach any stream.
std::string buildOverlayStream(const HeaderFooterConfig& cfg,
                               int pageIndexZeroBased,
                               int totalPages,
                               float pageWidthPt,
                               float pageHeightPt,
                               const std::string& isoDate,
                               const std::string& fontResourceName = "F1",
                               float originX = 0.0f,
                               float originY = 0.0f)
{
    auto resolve = [&](const std::string& raw) {
        return HeaderFooterEngine::resolveTokens(
            raw, pageIndexZeroBased, totalPages,
            cfg.author, cfg.filename, isoDate);
    };

    const std::string lh = resolve(cfg.leftHeader);
    const std::string ch = resolve(cfg.centerHeader);
    const std::string rh = resolve(cfg.rightHeader);
    const std::string lf = resolve(cfg.leftFooter);
    const std::string cf = resolve(cfg.centerFooter);
    const std::string rf = resolve(cfg.rightFooter);

    if (lh.empty() && ch.empty() && rh.empty() &&
        lf.empty() && cf.empty() && rf.empty()) {
        return {};
    }

    // Rough char-width estimate for centering/right-align: 0.5 * fontSize per char.
    const float charWidth = cfg.fontSize * 0.5f;
    const float yHeader = pageHeightPt - cfg.marginPt;
    const float yFooter = cfg.marginPt;

    std::ostringstream os;
    os << HeaderFooterEngine::kStartMarker << "\n";
    os << "q /" << fontResourceName << " " << cfg.fontSize << " Tf\n";
    // Reset ambient text/colour state to a known basis so a non-black ambient
    // fill can't render the header/footer invisible and ambient Tz/Tc/Tw/Ts
    // can't mirror or distort it (engine otherwise sets only Tf).
    os << pdfe::core::knownTextStateBasis();

    // Offset every placement by the MediaBox origin so header/footer land on
    // pages whose MediaBox does not start at (0,0).
    auto emit = [&](const std::string& text, float x, float y) {
        if (text.empty()) return;
        os << "BT " << (originX + x) << " " << (originY + y) << " Td ("
           << escapePdfLiteral(text) << ") Tj ET\n";
    };

    // Header row (top)
    emit(lh, cfg.marginPt, yHeader);
    if (!ch.empty()) {
        float w = ch.size() * charWidth;
        emit(ch, (pageWidthPt - w) / 2.0f, yHeader);
    }
    if (!rh.empty()) {
        float w = rh.size() * charWidth;
        emit(rh, pageWidthPt - cfg.marginPt - w, yHeader);
    }

    // Footer row (bottom)
    emit(lf, cfg.marginPt, yFooter);
    if (!cf.empty()) {
        float w = cf.size() * charWidth;
        emit(cf, (pageWidthPt - w) / 2.0f, yFooter);
    }
    if (!rf.empty()) {
        float w = rf.size() * charWidth;
        emit(rf, pageWidthPt - cfg.marginPt - w, yFooter);
    }

    os << "Q\n";
    os << HeaderFooterEngine::kEndMarker << "\n";
    return os.str();
}

// True if `fontObj` is a simple Helvetica Type1 font we can safely reuse for
// header/footer WinAnsi text.
bool isHelveticaType1(fz_context* ctx, pdf_obj* fontObj)
{
    if (!fontObj) return false;
    pdf_obj* sub = pdf_dict_get(ctx, fontObj, PDF_NAME(Subtype));
    pdf_obj* base = pdf_dict_get(ctx, fontObj, PDF_NAME(BaseFont));
    const char* subName = sub ? pdf_to_name(ctx, sub) : "";
    const char* baseName = base ? pdf_to_name(ctx, base) : "";
    return subName && std::strcmp(subName, "Type1") == 0 &&
           baseName && std::strcmp(baseName, "Helvetica") == 0;
}

// Choose a /Font resource key for the header/footer Helvetica WITHOUT mutating
// the page. Reuses an existing "F1" only when it is already Helvetica Type1;
// otherwise a page whose "F1" is (say) a Type0/CID body font would make the
// header/footer WinAnsi bytes render as the wrong glyphs. In that case pick the
// first free "HFn" key.
std::string chooseHelveticaResourceName(fz_context* ctx, pdf_page* page)
{
    pdf_obj* resources = pdf_dict_get(ctx, page->obj, PDF_NAME(Resources));
    pdf_obj* fonts = resources ? pdf_dict_get(ctx, resources, PDF_NAME(Font)) : nullptr;
    if (!fonts) return "F1";
    pdf_obj* f1 = pdf_dict_gets(ctx, fonts, "F1");
    if (!f1) return "F1";                       // free — use it
    if (isHelveticaType1(ctx, f1)) return "F1"; // safe to reuse
    for (int i = 0; ; ++i) {                    // F1 taken by another font
        std::string cand = "HF" + std::to_string(i);
        if (!pdf_dict_gets(ctx, fonts, cand.c_str())) return cand;
    }
}

// Ensure page has a /Resources /Font /<name> Helvetica entry (adds it if the
// key is free; leaves an existing reusable Helvetica untouched).
void ensureHelveticaResource(fz_context* ctx, pdf_document* doc, pdf_page* page,
                             const std::string& name)
{
    pdf_obj* resources = pdf_dict_get(ctx, page->obj, PDF_NAME(Resources));
    if (!resources) {
        resources = pdf_new_dict(ctx, doc, 2);
        pdf_dict_put(ctx, page->obj, PDF_NAME(Resources), resources);
    }
    pdf_obj* fonts = pdf_dict_get(ctx, resources, PDF_NAME(Font));
    if (!fonts) {
        fonts = pdf_new_dict(ctx, doc, 1);
        pdf_dict_put(ctx, resources, PDF_NAME(Font), fonts);
    }
    if (pdf_dict_gets(ctx, fonts, name.c_str())) return;

    pdf_obj* f = pdf_new_dict(ctx, doc, 4);
    pdf_dict_put(ctx, f, PDF_NAME(Type), PDF_NAME(Font));
    pdf_dict_put(ctx, f, PDF_NAME(Subtype), PDF_NAME(Type1));
    pdf_dict_put_name(ctx, f, PDF_NAME(BaseFont), "Helvetica");
    pdf_dict_put_name(ctx, f, PDF_NAME(Encoding), "WinAnsiEncoding");
    pdf_dict_puts(ctx, fonts, name.c_str(), f);
}

// Returns true if the given stream object is a marker-tagged H/F stream.
bool streamIsMarked(fz_context* ctx, pdf_obj* streamObj)
{
    if (!streamObj) return false;
    fz_buffer* buf = nullptr;
    fz_try(ctx) {
        buf = pdf_load_stream(ctx, streamObj);
    } fz_catch(ctx) {
        buf = nullptr;
    }
    if (!buf) return false;
    unsigned char* data = nullptr;
    size_t len = fz_buffer_storage(ctx, buf, &data);
    // Look for the start marker anywhere in the stream prefix.
    bool marked = false;
    if (data && len > 0) {
        const std::string marker = HeaderFooterEngine::kStartMarker;
        size_t scan = std::min<size_t>(len, 256);
        for (size_t i = 0; i + marker.size() <= scan; ++i) {
            if (std::memcmp(data + i, marker.data(), marker.size()) == 0) {
                marked = true;
                break;
            }
        }
    }
    fz_drop_buffer(ctx, buf);
    return marked;
}

// Strip marker-tagged streams from a page's /Contents array.
void stripMarkedFromPage(fz_context* ctx, pdf_document* doc, pdf_page* page)
{
    pdf_obj* contents = pdf_dict_get(ctx, page->obj, PDF_NAME(Contents));
    if (!contents) return;

    if (!pdf_is_array(ctx, contents)) {
        // Single stream — drop it entirely if marked.
        if (streamIsMarked(ctx, contents)) {
            pdf_dict_del(ctx, page->obj, PDF_NAME(Contents));
        }
        return;
    }

    int n = pdf_array_len(ctx, contents);
    // Walk from end so index removal is stable.
    for (int i = n - 1; i >= 0; --i) {
        pdf_obj* entry = pdf_array_get(ctx, contents, i);
        if (streamIsMarked(ctx, entry)) {
            pdf_array_delete(ctx, contents, i);
        }
    }
    // Collapse empty array.
    if (pdf_array_len(ctx, contents) == 0) {
        pdf_dict_del(ctx, page->obj, PDF_NAME(Contents));
    }
}

void appendContentStream(fz_context* ctx, pdf_document* doc, pdf_page* page,
                         const std::string& text)
{
    // Append so the header/footer runs at IDENTITY CTM even on N-up / imposed
    // pages: appendContentIsolatingAmbientCtm brackets the existing page
    // content in `q ... Q`, so its outer `Q` restores the page-initial identity
    // CTM before this fragment runs. A bare append would inherit an ambient
    // `cm` (e.g. "0.5 0 0 0.5 0 0 cm") and render the text half-size. The
    // fragment is itself balanced `q ... Q`, so the guard is a no-op for the
    // OUTBOUND direction. (Register H1 / content-stream-state-continuity audit,
    // pipeline content-stream-continuity phase-2.)
    pdfe::core::appendContentIsolatingAmbientCtm(
        ctx, doc, page->obj, text.data(), text.size());
}

} // namespace

// ---------- Public ops ----------

void HeaderFooterEngine::apply(const std::filesystem::path& input,
                               const std::filesystem::path& output,
                               const HeaderFooterConfig& config)
{
    if (pdfe::core::path_security::is_unc_or_remote(input.string())) {
        throw std::runtime_error(
            "HeaderFooterEngine::apply: refusing UNC / network-scheme input: "
            + input.string());
    }

    const std::string isoDate = isoDateToday();

    impl_->ctx->execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_open_document(ctx, input.string().c_str());
        fz_try(ctx) {
            // F24 REMEDIATION (security-audit gate, finding 1). Header/footer
            // apply and remove both rewrite page CONTENT STREAMS —
            // DocMdpChangeClass::Other, permitted at no certification level
            // (ISO 32000-1 Table 254). This engine never goes through
            // pdfe::core::Document, so it never met the F24 save guard and
            // could overwrite a certified original in place unannounced.
            // Same policy: refuse in place, warn and proceed elsewhere.
            char refusal[512] = {0};
            if (pdfe::signatures::directWriteRefused(
                    ctx, doc, input, output,
                    pdfe::signatures::DocMdpChangeClass::Other,
                    refusal, sizeof(refusal))) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "%s", refusal);
            }

            int total = pdf_count_pages(ctx, doc);
            for (int i = 0; i < total; ++i) {
                pdf_page* page = pdfe::core::safeLoadPdfPage(ctx, doc, i);
                fz_try(ctx) {
                    // Always strip prior marker streams so re-apply doesn't stack.
                    stripMarkedFromPage(ctx, doc, page);

                    if (!HeaderFooterEngine::pageInRange(i, total, config)) {
                        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
                        continue;
                    }

                    fz_rect mb = pdf_bound_page(ctx, page, FZ_MEDIA_BOX);
                    float w = mb.x1 - mb.x0;
                    float h = mb.y1 - mb.y0;

                    // pdf_bound_page normalises the box to (0,0); the content
                    // coordinate system is absolute, so read the raw MediaBox
                    // origin from the page dict to place header/footer on pages
                    // whose MediaBox does not start at (0,0).
                    float mbOx = 0.0f, mbOy = 0.0f;
                    {
                        pdf_obj* mbArr = pdf_dict_get_inheritable(
                            ctx, page->obj, PDF_NAME(MediaBox));
                        if (mbArr && pdf_array_len(ctx, mbArr) == 4) {
                            mbOx = pdf_to_real(ctx, pdf_array_get(ctx, mbArr, 0));
                            mbOy = pdf_to_real(ctx, pdf_array_get(ctx, mbArr, 1));
                        }
                    }

                    // Pick a collision-free Helvetica key.
                    std::string fontRes = chooseHelveticaResourceName(ctx, page);
                    std::string stream = buildOverlayStream(
                        config, i, total, w, h, isoDate, fontRes, mbOx, mbOy);
                    if (stream.empty()) {
                        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
                        continue;
                    }

                    ensureHelveticaResource(ctx, doc, page, fontRes);
                    appendContentStream(ctx, doc, page, stream);
                } fz_always(ctx) {
                    fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
                } fz_catch(ctx) {
                    fz_rethrow(ctx);
                }
            }

            pdf_save_document(ctx, doc, output.string().c_str(), nullptr);
        } fz_always(ctx) {
            pdf_drop_document(ctx, doc);
        } fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });
}

void HeaderFooterEngine::remove(const std::filesystem::path& input,
                                const std::filesystem::path& output)
{
    if (pdfe::core::path_security::is_unc_or_remote(input.string())) {
        throw std::runtime_error(
            "HeaderFooterEngine::remove: refusing UNC / network-scheme input: "
            + input.string());
    }

    impl_->ctx->execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_open_document(ctx, input.string().c_str());
        fz_try(ctx) {
            // F24 REMEDIATION (security-audit gate, finding 1). Header/footer
            // apply and remove both rewrite page CONTENT STREAMS —
            // DocMdpChangeClass::Other, permitted at no certification level
            // (ISO 32000-1 Table 254). This engine never goes through
            // pdfe::core::Document, so it never met the F24 save guard and
            // could overwrite a certified original in place unannounced.
            // Same policy: refuse in place, warn and proceed elsewhere.
            char refusal[512] = {0};
            if (pdfe::signatures::directWriteRefused(
                    ctx, doc, input, output,
                    pdfe::signatures::DocMdpChangeClass::Other,
                    refusal, sizeof(refusal))) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "%s", refusal);
            }

            int total = pdf_count_pages(ctx, doc);
            for (int i = 0; i < total; ++i) {
                pdf_page* page = pdfe::core::safeLoadPdfPage(ctx, doc, i);
                fz_try(ctx) {
                    stripMarkedFromPage(ctx, doc, page);
                } fz_always(ctx) {
                    fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
                } fz_catch(ctx) {
                    fz_rethrow(ctx);
                }
            }
            pdf_save_document(ctx, doc, output.string().c_str(), nullptr);
        } fz_always(ctx) {
            pdf_drop_document(ctx, doc);
        } fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });
}

} // namespace pdfe::headerfooter
