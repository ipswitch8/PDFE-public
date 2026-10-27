#include "PageLabels.h"

#include "Document.h"
#include "MuPdfContext.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace pdfe::core {

namespace {

// Format n in PDF spec page-label conventions.
//   Decimal: "n"
//   RomanUpper / RomanLower: classic Roman numeral; numbers > 3999 get
//     repeated 'M's (matching Acrobat's behavior).
//   AlphaUpper / AlphaLower: 'A','B',...,'Z','AA',...,'ZZ','AAA',...
std::string formatRoman(int n, bool upper)
{
    if (n <= 0) return std::string();
    static const std::pair<int, const char*> upperRoman[] = {
        {1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"},
        {100, "C"},  {90, "XC"},  {50, "L"},  {40, "XL"},
        {10, "X"},   {9, "IX"},   {5, "V"},   {4, "IV"},
        {1, "I"}
    };
    static const std::pair<int, const char*> lowerRoman[] = {
        {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"},
        {100, "c"},  {90, "xc"},  {50, "l"},  {40, "xl"},
        {10, "x"},   {9, "ix"},   {5, "v"},   {4, "iv"},
        {1, "i"}
    };
    std::string out;
    const auto& table = upper ? upperRoman : lowerRoman;
    for (const auto& p : table) {
        while (n >= p.first) {
            out += p.second;
            n -= p.first;
        }
    }
    return out;
}

std::string formatAlpha(int n, bool upper)
{
    if (n <= 0) return std::string();
    // Per PDF spec: A, B, ..., Z, AA, BB, CC, ... ZZ, AAA, ...
    // i.e. block index = (n-1) / 26, char index = (n-1) % 26, repeated
    // (block index + 1) times.
    const int idx = (n - 1) % 26;
    const int reps = (n - 1) / 26 + 1;
    const char base = upper ? 'A' : 'a';
    return std::string(static_cast<size_t>(reps),
                       static_cast<char>(base + idx));
}

} // namespace

char PageLabelManager::styleToPdfChar(PageLabelStyle style)
{
    switch (style) {
        case PageLabelStyle::Decimal:    return 'D';
        case PageLabelStyle::RomanUpper: return 'R';
        case PageLabelStyle::RomanLower: return 'r';
        case PageLabelStyle::AlphaUpper: return 'A';
        case PageLabelStyle::AlphaLower: return 'a';
        case PageLabelStyle::None:       return '\0';
    }
    return '\0';
}

PageLabelStyle PageLabelManager::pdfCharToStyle(char c)
{
    switch (c) {
        case 'D': return PageLabelStyle::Decimal;
        case 'R': return PageLabelStyle::RomanUpper;
        case 'r': return PageLabelStyle::RomanLower;
        case 'A': return PageLabelStyle::AlphaUpper;
        case 'a': return PageLabelStyle::AlphaLower;
        default:  return PageLabelStyle::None;
    }
}

std::string PageLabelManager::formatNumber(int n, PageLabelStyle style)
{
    switch (style) {
        case PageLabelStyle::Decimal:
            return std::to_string(n);
        case PageLabelStyle::RomanUpper: return formatRoman(n, true);
        case PageLabelStyle::RomanLower: return formatRoman(n, false);
        case PageLabelStyle::AlphaUpper: return formatAlpha(n, true);
        case PageLabelStyle::AlphaLower: return formatAlpha(n, false);
        case PageLabelStyle::None:       return std::string();
    }
    return std::string();
}

std::string PageLabelManager::labelFor(
    int pageIndex,
    const std::vector<PageLabelRange>& rangesIn)
{
    if (rangesIn.empty() || pageIndex < 0) return std::string();

    // Sort ranges by firstPage; the active range is the last one with
    // firstPage <= pageIndex.
    std::vector<PageLabelRange> ranges = rangesIn;
    std::sort(ranges.begin(), ranges.end(),
              [](const PageLabelRange& a, const PageLabelRange& b) {
                  return a.firstPage < b.firstPage;
              });

    const PageLabelRange* active = nullptr;
    for (const auto& r : ranges) {
        if (r.firstPage <= pageIndex) active = &r;
        else break;
    }
    if (!active) return std::string();

    const int n = active->startValue + (pageIndex - active->firstPage);
    return active->prefix + formatNumber(n, active->style);
}

std::vector<PageLabelRange> PageLabelManager::read(Document* doc)
{
    std::vector<PageLabelRange> out;
    if (!doc || !doc->fzDocument()) return out;

    auto ctx = doc->context();
    ctx->execute([&](fz_context* c) {
        pdf_document* pdoc = pdf_document_from_fz_document(c, doc->fzDocument());
        if (!pdoc) return;
        pdf_obj* root = pdf_dict_get(c, pdf_trailer(c, pdoc), PDF_NAME(Root));
        if (!root) return;
        pdf_obj* pl = pdf_dict_get(c, root, PDF_NAME(PageLabels));
        if (!pl) return;
        // /PageLabels is a number tree; the simplest viable form is
        // /Nums [int dict int dict ...]. Walk that array directly.
        pdf_obj* nums = pdf_dict_get(c, pl, PDF_NAME(Nums));
        if (!nums || !pdf_is_array(c, nums)) return;
        const int n = pdf_array_len(c, nums);
        for (int i = 0; i + 1 < n; i += 2) {
            pdf_obj* idxObj = pdf_array_get(c, nums, i);
            pdf_obj* dict   = pdf_array_get(c, nums, i + 1);
            if (!pdf_is_number(c, idxObj) || !pdf_is_dict(c, dict)) continue;

            PageLabelRange r;
            r.firstPage = pdf_to_int(c, idxObj);

            pdf_obj* sObj = pdf_dict_get(c, dict, PDF_NAME(S));
            if (sObj && pdf_is_name(c, sObj)) {
                const char* name = pdf_to_name(c, sObj);
                if (name && std::strlen(name) >= 1) {
                    r.style = PageLabelManager::pdfCharToStyle(name[0]);
                }
            } else {
                r.style = PageLabelStyle::None;
            }

            pdf_obj* pObj = pdf_dict_get(c, dict, PDF_NAME(P));
            if (pObj && pdf_is_string(c, pObj)) {
                const char* prefix = pdf_to_text_string(c, pObj);
                if (prefix) r.prefix = prefix;
            }

            pdf_obj* stObj = pdf_dict_get(c, dict, PDF_NAME(St));
            if (stObj && pdf_is_number(c, stObj)) {
                r.startValue = pdf_to_int(c, stObj);
                if (r.startValue < 1) r.startValue = 1;
            } else {
                r.startValue = 1;
            }

            out.push_back(r);
        }
    });

    std::sort(out.begin(), out.end(),
              [](const PageLabelRange& a, const PageLabelRange& b) {
                  return a.firstPage < b.firstPage;
              });
    return out;
}

void PageLabelManager::write(Document* doc, std::vector<PageLabelRange> ranges)
{
    if (!doc || !doc->fzDocument()) return;
    // Rewriting the /PageLabels number tree orphans the previous tree; force a
    // full-rewrite save (pipeline b2e47d91, Tier 1).
    doc->markRequiresFullRewrite();

    // Sort + clamp ranges to valid page indices.
    const int pageCount = doc->pageCount();
    for (auto& r : ranges) {
        if (r.firstPage < 0) r.firstPage = 0;
        if (pageCount > 0 && r.firstPage >= pageCount) {
            r.firstPage = pageCount - 1;
        }
        if (r.startValue < 1) r.startValue = 1;
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const PageLabelRange& a, const PageLabelRange& b) {
                  return a.firstPage < b.firstPage;
              });

    auto ctx = doc->context();
    ctx->execute([&](fz_context* c) {
        pdf_document* pdoc = pdf_document_from_fz_document(c, doc->fzDocument());
        if (!pdoc) return;
        pdf_obj* root = pdf_dict_get(c, pdf_trailer(c, pdoc), PDF_NAME(Root));
        if (!root) return;

        if (ranges.empty()) {
            pdf_dict_del(c, root, PDF_NAME(PageLabels));
            return;
        }

        pdf_obj* pl = pdf_new_dict(c, pdoc, 1);
        pdf_obj* nums = pdf_new_array(c, pdoc, ranges.size() * 2);
        for (const auto& r : ranges) {
            pdf_array_push_int(c, nums, r.firstPage);

            pdf_obj* dict = pdf_new_dict(c, pdoc, 3);
            const char ch = PageLabelManager::styleToPdfChar(r.style);
            if (ch != '\0') {
                const char nameStr[2] = {ch, '\0'};
                pdf_dict_put_name(c, dict, PDF_NAME(S), nameStr);
            }
            if (!r.prefix.empty()) {
                pdf_dict_put_text_string(c, dict, PDF_NAME(P),
                                         r.prefix.c_str());
            }
            if (r.startValue != 1) {
                pdf_dict_put_int(c, dict, PDF_NAME(St), r.startValue);
            }
            pdf_array_push_drop(c, nums, dict);
        }
        pdf_dict_put_drop(c, pl, PDF_NAME(Nums), nums);
        pdf_dict_put_drop(c, root, PDF_NAME(PageLabels), pl);
    });
}

// ---------- EditPageLabelsCommand ----------

EditPageLabelsCommand::EditPageLabelsCommand(Document* doc,
                                             std::vector<PageLabelRange> newRanges)
    : doc_(doc), newRanges_(std::move(newRanges)) {}

void EditPageLabelsCommand::execute()
{
    // Capture the pre-edit ranges exactly once, on the first execute, BEFORE
    // overwriting them — so undo() restores the original document state.
    if (!captured_) {
        oldRanges_ = PageLabelManager::read(doc_);
        captured_ = true;
    }
    PageLabelManager::write(doc_, newRanges_);
}

void EditPageLabelsCommand::undo()
{
    PageLabelManager::write(doc_, oldRanges_);
}

} // namespace pdfe::core
