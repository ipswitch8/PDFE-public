#include "SearchEngine.h"
#include "Document.h"
#include "Page.h"
#include "MuPdfContext.h"
#include "Error.h"

#include <mupdf/fitz.h>
#include <algorithm>
#include <future>
#include <QThreadPool>
#include <QRunnable>

namespace pdfe::core {

namespace {

/// Byte offset of the `n`th (zero-based) CASE-FOLDED occurrence of `query` in
/// `text`, or std::string::npos if there is no such occurrence.
size_t nthFoldedOccurrence(const std::string& text, const std::string& query,
                           size_t n) {
    if (query.empty()) return std::string::npos;
    std::string lowerText = text;
    std::string lowerQuery = query;
    std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), ::tolower);
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);

    size_t pos = std::string::npos;
    size_t from = 0;
    for (size_t i = 0; i <= n; ++i) {
        pos = lowerText.find(lowerQuery, from);
        if (pos == std::string::npos) return std::string::npos;
        from = pos + lowerQuery.size();
    }
    return pos;
}

/// True when the `occurrence`th case-folded match in `pageText` also matches
/// `query` EXACTLY (same case). Used to honour SearchFlags::CaseSensitive, which
/// MuPDF's always-folding fz_search_page cannot express (gap A3-16).
///
/// If the occurrence cannot be located in the page's plain text at all the hit
/// is KEPT: the page text and MuPDF's internal stext walk can disagree about
/// whitespace normalisation, and dropping a real match on a bookkeeping mismatch
/// would be a silent false negative — the worse of the two errors.
bool occurrenceMatchesCase(const std::string& pageText, const std::string& query,
                           size_t occurrence) {
    const size_t pos = nthFoldedOccurrence(pageText, query, occurrence);
    if (pos == std::string::npos) return true;
    return pageText.compare(pos, query.size(), query) == 0;
}

}  // namespace

/// Runnable for async search execution on Qt thread pool.
class SearchTask : public QRunnable {
public:
    SearchTask(
        Document* doc,
        std::string query,
        SearchFlags flags,
        std::function<void(int, int)> progressCb,
        std::function<void(SearchResult)> completionCb,
        std::shared_ptr<std::atomic<bool>> cancelFlag
    )
        : document(doc)
        , query(std::move(query))
        , flags(flags)
        , progressCallback(std::move(progressCb))
        , completionCallback(std::move(completionCb))
        , cancelled(std::move(cancelFlag))
    {
        setAutoDelete(true);
    }

    void run() override {
        SearchResult result;
        result.query = query;
        result.caseSensitive = hasFlag(flags, SearchFlags::CaseSensitive);

        const int pageCount = document->pageCount();
        auto context = document->context();

        for (int pageIdx = 0; pageIdx < pageCount; ++pageIdx) {
            if (cancelled && cancelled->load()) {
                // Search was cancelled
                return;
            }

            // Load the page
            auto page = document->page(pageIdx);
            if (!page) continue;

            // Search this page using MuPDF's fz_search_page
            std::vector<SearchHit> pageHits = searchPage(context, page.get(), pageIdx, query, flags);
            result.hits.insert(result.hits.end(), pageHits.begin(), pageHits.end());

            // Report progress
            if (progressCallback) {
                progressCallback(pageIdx + 1, pageCount);
            }
        }

        // Call completion callback
        if (completionCallback && !(cancelled && cancelled->load())) {
            completionCallback(std::move(result));
        }
    }

private:
    static std::vector<SearchHit> searchPage(
        std::shared_ptr<MuPdfContext> context,
        Page* page,
        int pageIdx,
        const std::string& query,
        SearchFlags flags
    ) {
        std::vector<SearchHit> hits;

        // Get the fz_page handle (via SearchEngine friend access)
        ::fz_page* fzPage = SearchEngine::getPageHandle(page);
        if (!fzPage) return hits;

        // Prepare the search needle
        std::string needle = query;
        if (!hasFlag(flags, SearchFlags::CaseSensitive)) {
            std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
        }

        // Search the page using MuPDF.
        //
        // fz_search_page returns the number of QUADS, not the number of MATCHES:
        // one match spans as many quads as it does line/style runs. The `hit_mark`
        // out-array is how MuPDF tells the two apart — a non-zero entry marks the
        // quad that BEGINS a new match. This used to be passed as nullptr and
        // every quad on the page was then folded into a single SearchHit, so
        // SearchResult::totalMatches() actually counted PAGES-WITH-A-MATCH, not
        // matches: a page containing "NEEDLE" six times reported one hit, the UI
        // match counter under-reported, and next/previous-match could not step
        // between occurrences on the same page. Grouping by hit_mark below is the
        // fix (gap A3-16).
        constexpr int MAX_QUADS_PER_HIT = 512;
        fz_quad quads[MAX_QUADS_PER_HIT];
        int marks[MAX_QUADS_PER_HIT];
        std::fill(std::begin(marks), std::end(marks), 0);

        int quadCount = context->executeReturn<int>(
            [fzPage, &needle, &quads, &marks](fz_context* ctx) -> int {
                return fz_search_page(ctx, fzPage, needle.c_str(), marks, quads, MAX_QUADS_PER_HIT);
            }
        );

        if (quadCount == 0) {
            return hits;
        }

        // Get page plain text for context extraction.
        //
        // PERMISSION NOTE (a11y-bypass sweep): deliberately NOT gated on
        // copyAllowed()/extractForAccessibilityAllowed(). Search-within-document
        // is an internal operation on a document the user already has open and
        // can already see rendered; the text never leaves the process (no file,
        // no clipboard, no IPC/UIA surface) — it only produces the bounded
        // snippet shown beside the hit the user is being scrolled to. The gated
        // paths are the ones that MATERIALISE content outward: the exporters,
        // the clipboard (MainWindow::onCopy / onSelectAll via
        // documentDeniesCopy()) and the accessibility tree
        // (src/ui/PdfCanvasAccessible.cpp pageText()).
        std::string pageText = page->extractPlainText();

        // Convert quads to QRectF and group them into hits. A hit starts at quad
        // 0 and at every quad whose hit_mark entry is non-zero; the quads in
        // between belong to the same match (a word broken across lines or style
        // runs). matchStart/matchEnd are the ordinal of the occurrence within the
        // page text so callers can step occurrence-by-occurrence.
        //
        // CASE SENSITIVITY (gap A3-16): fz_search_page is ALWAYS case-folding —
        // there is no case-sensitive variant in the MuPDF search API. The
        // SearchFlags::CaseSensitive flag used to be recorded on the result and
        // then ignored, so a case-sensitive search returned the case-INsensitive
        // hit set (the only test over it asserted that the flag was *stored*).
        // MuPDF walks the stext page in reading order and so do we, so hit n
        // corresponds to the nth case-folded occurrence in the page's plain text;
        // that lets the true case be checked and non-matching hits dropped.
        const bool caseSensitive = hasFlag(flags, SearchFlags::CaseSensitive);
        size_t occurrence = 0;
        std::vector<QRectF> currentHitQuads;
        currentHitQuads.reserve(quadCount);

        auto flushHit = [&]() {
            if (currentHitQuads.empty()) return;
            const size_t thisOccurrence = occurrence++;
            if (caseSensitive &&
                !occurrenceMatchesCase(pageText, query, thisOccurrence)) {
                currentHitQuads.clear();
                return;
            }
            SearchHit hit;
            hit.pageIndex = pageIdx;
            hit.quads = std::move(currentHitQuads);
            currentHitQuads.clear();
            hit.matchStart = thisOccurrence;
            hit.matchEnd = thisOccurrence + query.size();
            hit.context = extractContext(pageText, query, thisOccurrence);
            hits.push_back(std::move(hit));
        };

        for (int i = 0; i < quadCount; ++i) {
            if (i > 0 && marks[i] != 0) {
                flushHit();
            }
            const fz_quad& q = quads[i];

            // Convert fz_quad to QRectF
            float minX = std::min({q.ul.x, q.ur.x, q.ll.x, q.lr.x});
            float maxX = std::max({q.ul.x, q.ur.x, q.ll.x, q.lr.x});
            float minY = std::min({q.ul.y, q.ur.y, q.ll.y, q.lr.y});
            float maxY = std::max({q.ul.y, q.ur.y, q.ll.y, q.lr.y});

            QRectF rect(minX, minY, maxX - minX, maxY - minY);
            currentHitQuads.push_back(rect);
        }
        flushHit();

        return hits;
    }

    /// @param occurrence zero-based ordinal of the match within the page text.
    ///        Previously ignored, so every hit on a page carried the context of
    ///        the FIRST occurrence (part of the A3-16 single-hit-per-page defect).
    static std::string extractContext(const std::string& pageText, const std::string& query, size_t occurrence) {
        constexpr size_t CONTEXT_CHARS = 50;

        // Find the occurrence-th case-folded match in the text.
        const std::string lowerQuery = query;
        const size_t foundPos = nthFoldedOccurrence(pageText, query, occurrence);
        if (foundPos == std::string::npos) {
            // Fallback: just return beginning of page
            return pageText.substr(0, std::min(CONTEXT_CHARS, pageText.size())) + "...";
        }

        // Extract context around the match
        size_t start = (foundPos > CONTEXT_CHARS / 2) ? foundPos - CONTEXT_CHARS / 2 : 0;
        size_t end = std::min(foundPos + lowerQuery.size() + CONTEXT_CHARS / 2, pageText.size());

        std::string context;
        if (start > 0) context += "...";
        context += pageText.substr(start, end - start);
        if (end < pageText.size()) context += "...";

        return context;
    }

    Document* document;
    std::string query;
    SearchFlags flags;
    std::function<void(int, int)> progressCallback;
    std::function<void(SearchResult)> completionCallback;
    std::shared_ptr<std::atomic<bool>> cancelled;
};

// SearchEngine::Impl
class SearchEngine::Impl {
public:
    Document* document = nullptr;
    std::shared_ptr<std::atomic<bool>> cancelFlag;

    explicit Impl(Document* doc) : document(doc) {
        if (!doc) {
            throw PdfeException("Document cannot be null");
        }
    }

    SearchResult search(const std::string& query, SearchFlags flags) {
        SearchResult result;
        result.query = query;
        result.caseSensitive = hasFlag(flags, SearchFlags::CaseSensitive);

        const int pageCount = document->pageCount();
        auto context = document->context();

        for (int pageIdx = 0; pageIdx < pageCount; ++pageIdx) {
            // Load the page
            auto page = document->page(pageIdx);
            if (!page) continue;

            // Search this page
            std::vector<SearchHit> pageHits = searchPageSync(context, page.get(), pageIdx, query, flags);
            result.hits.insert(result.hits.end(), pageHits.begin(), pageHits.end());
        }

        return result;
    }

    SearchResult searchSpecificPages(const std::string& query,
                                     const std::vector<int>& pageIndices,
                                     SearchFlags flags) {
        SearchResult result;
        result.query = query;
        result.caseSensitive = hasFlag(flags, SearchFlags::CaseSensitive);

        const int pageCount = document->pageCount();
        auto context = document->context();

        // Visit each requested index once, in input order, ignoring OOB.
        std::vector<bool> visited(pageCount, false);
        for (int pageIdx : pageIndices) {
            if (pageIdx < 0 || pageIdx >= pageCount) continue;
            if (visited[pageIdx]) continue;
            visited[pageIdx] = true;
            auto page = document->page(pageIdx);
            if (!page) continue;
            std::vector<SearchHit> pageHits = searchPageSync(
                context, page.get(), pageIdx, query, flags);
            result.hits.insert(result.hits.end(),
                               pageHits.begin(), pageHits.end());
        }
        return result;
    }

    void searchAsync(
        const std::string& query,
        SearchFlags flags,
        std::function<void(int, int)> progressCb,
        std::function<void(SearchResult)> completionCb
    ) {
        // Reset cancel flag
        cancelFlag = std::make_shared<std::atomic<bool>>(false);

        // Create and schedule the search task
        auto* task = new SearchTask(
            document,
            query,
            flags,
            std::move(progressCb),
            std::move(completionCb),
            cancelFlag
        );

        QThreadPool::globalInstance()->start(task);
    }

    void cancelSearch() {
        if (cancelFlag) {
            cancelFlag->store(true);
        }
    }

private:
    static std::vector<SearchHit> searchPageSync(
        std::shared_ptr<MuPdfContext> context,
        Page* page,
        int pageIdx,
        const std::string& query,
        SearchFlags flags
    ) {
        std::vector<SearchHit> hits;

        // Get the fz_page handle (via SearchEngine friend access)
        ::fz_page* fzPage = SearchEngine::getPageHandle(page);
        if (!fzPage) return hits;

        // Prepare the search needle
        std::string needle = query;
        if (!hasFlag(flags, SearchFlags::CaseSensitive)) {
            std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
        }

        // Search the page using MuPDF. See searchPage() above for why hit_mark is
        // passed rather than nullptr (gap A3-16: quads != matches).
        constexpr int MAX_QUADS_PER_HIT = 512;
        fz_quad quads[MAX_QUADS_PER_HIT];
        int marks[MAX_QUADS_PER_HIT];
        std::fill(std::begin(marks), std::end(marks), 0);

        int quadCount = context->executeReturn<int>(
            [fzPage, &needle, &quads, &marks](fz_context* ctx) -> int {
                return fz_search_page(ctx, fzPage, needle.c_str(), marks, quads, MAX_QUADS_PER_HIT);
            }
        );

        if (quadCount == 0) {
            return hits;
        }

        // Get page plain text for context extraction.
        //
        // PERMISSION NOTE (a11y-bypass sweep): deliberately NOT gated on
        // copyAllowed()/extractForAccessibilityAllowed(). Search-within-document
        // is an internal operation on a document the user already has open and
        // can already see rendered; the text never leaves the process (no file,
        // no clipboard, no IPC/UIA surface) — it only produces the bounded
        // snippet shown beside the hit the user is being scrolled to. The gated
        // paths are the ones that MATERIALISE content outward: the exporters,
        // the clipboard (MainWindow::onCopy / onSelectAll via
        // documentDeniesCopy()) and the accessibility tree
        // (src/ui/PdfCanvasAccessible.cpp pageText()).
        std::string pageText = page->extractPlainText();

        // Convert quads to QRectF and group them into hits by hit_mark, then drop
        // the hits whose true case does not match a case-sensitive query. Both
        // are the A3-16 fix; see searchPage() above for the full rationale.
        const bool caseSensitive = hasFlag(flags, SearchFlags::CaseSensitive);
        size_t occurrence = 0;
        std::vector<QRectF> currentHitQuads;
        currentHitQuads.reserve(quadCount);

        auto flushHit = [&]() {
            if (currentHitQuads.empty()) return;
            const size_t thisOccurrence = occurrence++;
            if (caseSensitive &&
                !occurrenceMatchesCase(pageText, query, thisOccurrence)) {
                currentHitQuads.clear();
                return;
            }
            SearchHit hit;
            hit.pageIndex = pageIdx;
            hit.quads = std::move(currentHitQuads);
            currentHitQuads.clear();
            hit.matchStart = thisOccurrence;
            hit.matchEnd = thisOccurrence + query.size();
            hit.context = extractContextSync(pageText, query, thisOccurrence);
            hits.push_back(std::move(hit));
        };

        for (int i = 0; i < quadCount; ++i) {
            if (i > 0 && marks[i] != 0) {
                flushHit();
            }
            const fz_quad& q = quads[i];

            // Convert fz_quad to QRectF
            float minX = std::min({q.ul.x, q.ur.x, q.ll.x, q.lr.x});
            float maxX = std::max({q.ul.x, q.ur.x, q.ll.x, q.lr.x});
            float minY = std::min({q.ul.y, q.ur.y, q.ll.y, q.lr.y});
            float maxY = std::max({q.ul.y, q.ur.y, q.ll.y, q.lr.y});

            QRectF rect(minX, minY, maxX - minX, maxY - minY);
            currentHitQuads.push_back(rect);
        }
        flushHit();

        return hits;
    }

    /// @param occurrence zero-based ordinal of the match within the page text.
    static std::string extractContextSync(const std::string& pageText, const std::string& query, size_t occurrence) {
        constexpr size_t CONTEXT_CHARS = 50;

        // Find the occurrence-th case-folded match in the text.
        const std::string lowerQuery = query;
        const size_t foundPos = nthFoldedOccurrence(pageText, query, occurrence);
        if (foundPos == std::string::npos) {
            // Fallback: just return beginning of page
            return pageText.substr(0, std::min(CONTEXT_CHARS, pageText.size())) + "...";
        }

        // Extract context around the match
        size_t start = (foundPos > CONTEXT_CHARS / 2) ? foundPos - CONTEXT_CHARS / 2 : 0;
        size_t end = std::min(foundPos + lowerQuery.size() + CONTEXT_CHARS / 2, pageText.size());

        std::string context;
        if (start > 0) context += "...";
        context += pageText.substr(start, end - start);
        if (end < pageText.size()) context += "...";

        return context;
    }
};

// SearchEngine public interface
SearchEngine::SearchEngine(Document* document)
    : pImpl(std::make_unique<Impl>(document))
{
}

SearchEngine::~SearchEngine() = default;

SearchResult SearchEngine::search(const std::string& query, SearchFlags flags) {
    return pImpl->search(query, flags);
}

SearchResult SearchEngine::searchSpecificPages(const std::string& query,
                                               const std::vector<int>& pageIndices,
                                               SearchFlags flags) {
    return pImpl->searchSpecificPages(query, pageIndices, flags);
}

void SearchEngine::searchAsync(
    const std::string& query,
    SearchFlags flags,
    std::function<void(int, int)> progressCb,
    std::function<void(SearchResult)> completionCb
) {
    pImpl->searchAsync(query, flags, std::move(progressCb), std::move(completionCb));
}

void SearchEngine::cancelSearch() {
    pImpl->cancelSearch();
}

::fz_page* SearchEngine::getPageHandle(const Page* page) {
    return page ? page->fzPage() : nullptr;
}

} // namespace pdfe::core
