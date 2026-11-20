#include "ContentStreamTagger.h"
#include "../core/MuPdfContext.h"
#include "../core/Document.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QtGlobal>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace pdfe::compliance {

class ContentStreamTagger::Impl {
public:
    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> context)
        : context_(std::move(context))
    {}

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

ContentStreamTagger::ContentStreamTagger(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{}

ContentStreamTagger::~ContentStreamTagger() = default;

const char* artifactTypeToName(ArtifactType type) {
    switch (type) {
        case ArtifactType::Pagination: return "Pagination";
        case ArtifactType::Layout:     return "Layout";
        case ArtifactType::Page:       return "Page";
        case ArtifactType::Background: return "Background";
    }
    return "Pagination";
}

const char* artifactSubtypeToName(ArtifactSubtype subtype) {
    switch (subtype) {
        case ArtifactSubtype::Header:    return "Header";
        case ArtifactSubtype::Footer:    return "Footer";
        case ArtifactSubtype::Watermark: return "Watermark";
        case ArtifactSubtype::None:      break;
    }
    return nullptr;   // /Subtype omitted
}

namespace {

// "/Artifact <</Type /Pagination /Subtype /Header /BBox [x0 y0 x1 y1]>> BDC"
// — deliberately WITHOUT /MCID. Artifacts are not structure content; giving
// one an MCID would put page furniture into the ParentTree and hand it to a
// screen reader as document text, which is the exact failure this marking
// exists to prevent.
std::string artifactBdc(const ArtifactRegion& r) {
    std::ostringstream os;
    os << "/Artifact <</Type /" << artifactTypeToName(r.type);
    if (const char* sub = artifactSubtypeToName(r.subtype)) {
        os << " /Subtype /" << sub;
    }
    if (r.hasBBox) {
        os << " /BBox [" << r.bbox[0] << ' ' << r.bbox[1] << ' '
           << r.bbox[2] << ' ' << r.bbox[3] << ']';
    }
    os << ">> BDC\n";
    return os.str();
}

} // namespace

namespace {

// ── tagPageContent: one marked-content sequence per region (GitHub #438) ────
//
// The page content is read as a list of OPERATORS: an operator together with
// its operands, in content order, across every stream of /Contents. A region's
// [startOperatorIndex, endOperatorIndex) are indices into that list (end
// exclusive). An inline image (BI ... ID <data> EI) counts as one operator.
//
// The old code wrapped the WHOLE stream in region 0's BDC/EMC and appended the
// other regions as empty "BDC EMC" pairs, so the first element claimed every
// operator, the image included, and a /Figure MCID pointed at nothing.

struct ContentOp {
    std::size_t start = 0;  // first byte of the first operand (or of the operator)
    std::size_t end = 0;    // one past the operator keyword (or past EI)
    std::string keyword;
    std::string lastName;   // the last /Name operand (the XObject of a Do)
};

bool isPdfWhite(unsigned char c) {
    return c == 0 || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

bool isPdfDelim(unsigned char c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '/' || c == '%';
}

std::string decodePdfName(const std::string& raw) {
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '#' && i + 2 < raw.size() &&
            std::isxdigit(static_cast<unsigned char>(raw[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(raw[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(raw.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

// The ID operator of an inline image whose dictionary starts at `pos` (just
// after BI): the first bare-word token "ID", with the dictionary's names,
// strings, hex strings, arrays and nested dictionaries lexed as tokens on the
// way. A plain search for the two bytes "ID" stopped inside a key or value
// such as /IDx or (an ID). npos when there is none.
std::size_t findInlineImageId(const std::string& c, std::size_t pos) {
    const std::size_t n = c.size();
    while (pos < n) {
        const unsigned char ch = static_cast<unsigned char>(c[pos]);
        if (isPdfWhite(ch)) { ++pos; continue; }
        if (ch == '%') {
            while (pos < n && c[pos] != '\n' && c[pos] != '\r') ++pos;
            continue;
        }
        if (ch == '(') {
            int depth = 0;
            while (pos < n) {
                const char x = c[pos++];
                if (x == '\\') { ++pos; continue; }
                if (x == '(') ++depth;
                else if (x == ')' && --depth == 0) break;
            }
            continue;
        }
        if (ch == '<') {
            if (pos + 1 < n && c[pos + 1] == '<') { pos += 2; continue; }
            while (pos < n && c[pos] != '>') ++pos;
            if (pos < n) ++pos;
            continue;
        }
        if (isPdfDelim(ch)) {
            // '/' starts a name (its body is skipped below); '>' '[' ']' '{'
            // '}' ')' stand alone.
            ++pos;
            if (ch == '/') {
                while (pos < n && !isPdfWhite(static_cast<unsigned char>(c[pos])) &&
                       !isPdfDelim(static_cast<unsigned char>(c[pos])))
                    ++pos;
            }
            continue;
        }
        const std::size_t s = pos;
        while (pos < n && !isPdfWhite(static_cast<unsigned char>(c[pos])) &&
               !isPdfDelim(static_cast<unsigned char>(c[pos])))
            ++pos;
        if (pos - s == 2 && c[s] == 'I' && c[s + 1] == 'D') return s;
    }
    return std::string::npos;
}

// Tokenise `c` into operators. Strings (nested, escaped), hex strings,
// dictionaries, arrays, names and comments are operands or skipped; a bare
// word that is not a number, true, false or null is an operator.
std::vector<ContentOp> lexOperators(const std::string& c) {
    std::vector<ContentOp> ops;
    const std::size_t n = c.size();
    std::size_t pos = 0;
    std::size_t opStart = std::string::npos;
    std::string lastName;
    auto mark = [&](std::size_t at) {
        if (opStart == std::string::npos) opStart = at;
    };
    while (pos < n) {
        const unsigned char ch = static_cast<unsigned char>(c[pos]);
        if (isPdfWhite(ch)) { ++pos; continue; }
        if (ch == '%') {
            while (pos < n && c[pos] != '\n' && c[pos] != '\r') ++pos;
            continue;
        }
        if (ch == '(') {
            mark(pos);
            int depth = 0;
            while (pos < n) {
                const char x = c[pos++];
                if (x == '\\') { ++pos; continue; }
                if (x == '(') ++depth;
                else if (x == ')' && --depth == 0) break;
            }
            continue;
        }
        if (ch == '<') {
            mark(pos);
            if (pos + 1 < n && c[pos + 1] == '<') { pos += 2; continue; }
            while (pos < n && c[pos] != '>') ++pos;
            if (pos < n) ++pos;
            continue;
        }
        if (ch == '>') {
            mark(pos);
            pos += (pos + 1 < n && c[pos + 1] == '>') ? 2 : 1;
            continue;
        }
        if (ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == ')') {
            mark(pos);
            ++pos;
            continue;
        }
        if (ch == '/') {
            mark(pos);
            const std::size_t s = ++pos;
            while (pos < n && !isPdfWhite(static_cast<unsigned char>(c[pos])) &&
                   !isPdfDelim(static_cast<unsigned char>(c[pos])))
                ++pos;
            lastName = decodePdfName(c.substr(s, pos - s));
            continue;
        }
        // A bare word: a number, true/false/null, or an operator.
        const std::size_t s = pos;
        while (pos < n && !isPdfWhite(static_cast<unsigned char>(c[pos])) &&
               !isPdfDelim(static_cast<unsigned char>(c[pos])))
            ++pos;
        const std::string word = c.substr(s, pos - s);
        const unsigned char w0 = static_cast<unsigned char>(word[0]);
        if (std::isdigit(w0) || w0 == '+' || w0 == '-' || w0 == '.' || word == "true" ||
            word == "false" || word == "null") {
            mark(s);
            continue;
        }
        mark(s);
        ContentOp op;
        op.start = opStart;
        op.keyword = word;
        op.lastName = lastName;
        if (word == "BI") {
            // Inline image: lex the dictionary to the ID operator, then skip
            // the data to an EI that stands alone between white space (or the
            // end of the content).
            std::size_t idAt = findInlineImageId(c, pos);
            std::size_t p = (idAt == std::string::npos) ? n : idAt + 3;
            std::size_t endAt = n;
            while (p + 1 < n) {
                if (c[p] == 'E' && c[p + 1] == 'I' &&
                    isPdfWhite(static_cast<unsigned char>(c[p - 1])) &&
                    (p + 2 >= n || isPdfWhite(static_cast<unsigned char>(c[p + 2])))) {
                    endAt = p + 2;
                    break;
                }
                ++p;
            }
            pos = endAt;
        }
        op.end = pos;
        ops.push_back(std::move(op));
        opStart = std::string::npos;
        lastName.clear();
    }
    return ops;
}

// Nesting partners of q/Q, BT/ET and BDC|BMC/EMC: partner[i] is the index of
// the matching operator, -1 when unmatched, -2 when op i opens or closes
// nothing. mcDepth[i] is the marked-content depth BEFORE op i; qOpen[i] the
// innermost q open before op i (-1 if none).
struct Nesting {
    std::vector<int> partner;
    std::vector<int> mcDepth;
    std::vector<int> qOpen;
    std::vector<bool> inText;
};

Nesting computeNesting(const std::vector<ContentOp>& ops) {
    const int n = static_cast<int>(ops.size());
    Nesting nest;
    nest.partner.assign(n, -2);
    nest.mcDepth.assign(n, 0);
    nest.qOpen.assign(n, -1);
    nest.inText.assign(n, false);
    std::vector<int> qs, bts, mcs;
    for (int i = 0; i < n; ++i) {
        const std::string& k = ops[i].keyword;
        nest.mcDepth[i] = static_cast<int>(mcs.size());
        nest.qOpen[i] = qs.empty() ? -1 : qs.back();
        nest.inText[i] = !bts.empty();
        std::vector<int>* stack = nullptr;
        bool opener = false;
        if (k == "q") { stack = &qs; opener = true; }
        else if (k == "Q") { stack = &qs; }
        else if (k == "BT") { stack = &bts; opener = true; }
        else if (k == "ET") { stack = &bts; }
        else if (k == "BDC" || k == "BMC") { stack = &mcs; opener = true; }
        else if (k == "EMC") { stack = &mcs; }
        if (!stack) continue;
        if (opener) {
            nest.partner[i] = -1;
            stack->push_back(i);
        } else if (stack->empty()) {
            nest.partner[i] = -1;
        } else {
            const int o = stack->back();
            stack->pop_back();
            nest.partner[i] = o;
            nest.partner[o] = i;
        }
    }
    return nest;
}

// Matching-pair lookups for growing a span to its balanced hull.
//
// A hull is [s, e] grown until every q/BT/BDC inside it closes inside it and
// every Q/ET/EMC inside it opened inside it; there is none when the span holds
// an unmatched operator. It used to be found by rescanning [s, e] from s after
// every growth step, once per text atom, so a crafted stream with deep or many
// nested pairs made automatic placement super-quadratic (security-audit
// follow-up, phase 6). computeNesting already pairs every opener and closer in
// one stack pass; here one more pass builds a segment tree over those partner
// indices, so a growth step finds the first operator in [s, e] whose partner
// lies outside it in O(log n). A firm budget of growth steps per page bounds
// the work whatever the nesting. An unmatched operator's partner (-1) is below
// every span start, so the same query finds it, in the order the old scan did.
class HullIndex {
public:
    HullIndex(const Nesting& nest, long long budget)
        : partner_(nest.partner), budget_(budget) {
        const int n = static_cast<int>(partner_.size());
        while (size_ < n) size_ <<= 1;
        lo_.assign(static_cast<std::size_t>(2 * size_), std::numeric_limits<int>::max());
        hi_.assign(static_cast<std::size_t>(2 * size_), std::numeric_limits<int>::min());
        for (int i = 0; i < n; ++i) {
            if (partner_[i] == -2) continue;
            lo_[static_cast<std::size_t>(size_ + i)] = partner_[i];
            hi_[static_cast<std::size_t>(size_ + i)] = partner_[i];
        }
        for (int v = size_ - 1; v >= 1; --v) {
            lo_[v] = std::min(lo_[2 * v], lo_[2 * v + 1]);
            hi_[v] = std::max(hi_[2 * v], hi_[2 * v + 1]);
        }
    }

    // The first operator in [s, e] that opens or closes a pair reaching
    // outside [s, e], or that is unmatched; -1 when there is none.
    int firstEscaping(int s, int e) const {
        if (s > e || partner_.empty()) return -1;
        return find(1, 0, size_ - 1, s, e);
    }

    // [s, e] is balanced as it stands.
    bool isBalanced(int s, int e) const { return firstEscaping(s, e) < 0; }

    // Grow [s, e] to its balanced hull. False when that is impossible (an
    // unmatched operator; s and e then hold the span as grown so far) or when
    // the page's budget of growth steps is spent (exhausted() is then true).
    bool hull(int& s, int& e) {
        for (;;) {
            if (++work_ > budget_) {
                exhausted_ = true;
                return false;
            }
            const int k = firstEscaping(s, e);
            if (k < 0) return true;
            const int p = partner_[k];
            if (p == -1) return false;
            if (p < s) s = p;
            else e = p;
        }
    }

    bool exhausted() const { return exhausted_; }

private:
    int find(int v, int l, int r, int s, int e) const {
        if (r < s || l > e) return -1;
        if (lo_[v] >= s && hi_[v] <= e) return -1;
        if (l == r) return l;
        const int m = l + (r - l) / 2;
        const int left = find(2 * v, l, m, s, e);
        return left >= 0 ? left : find(2 * v + 1, m + 1, r, s, e);
    }

    const std::vector<int>& partner_;
    int size_ = 1;
    std::vector<int> lo_, hi_;
    long long budget_ = 0;
    long long work_ = 0;
    bool exhausted_ = false;
};

// Growth steps allowed per page. A step is O(log n) and well-formed content
// needs a few per text atom (an atom is at least two operators), so this is
// far above any real page while it bounds a hostile one. Past it the page is
// left untagged.
long long hullBudgetFor(std::size_t ops) {
    return 16LL * static_cast<long long>(ops) + 4096;
}

struct Span {
    int s = 0;
    int e = 0;
    std::size_t region = 0;
};

// Automatic placement, for callers that give no operator ranges (AutoTagger
// knows a block's kind and order, not its operators):
//   * each image draw (an image XObject Do, or an inline image), together with
//     a placement wrapper that holds nothing else, goes to the next /Figure
//     region, in content order;
//   * the text objects (BT ... ET) and Form XObject draws go to the other
//     non-artifact regions, in content order, as evenly as their nesting
//     allows; a text region's sequence never encloses an image draw.
// Only content outside any existing marked-content sequence is placed.
std::vector<Span> autoPlace(const std::vector<ContentOp>& ops, const Nesting& nest,
                            HullIndex& hulls,
                            const std::vector<MarkedContentRegion>& regions,
                            const std::set<std::string>& imageNames,
                            const std::set<std::string>& formNames) {
    std::vector<Span> spans;
    const int n = static_cast<int>(ops.size());
    std::vector<std::size_t> figures, texts;
    for (std::size_t r = 0; r < regions.size(); ++r) {
        if (regions[r].tag == StructureTag::Artifact) continue;
        (regions[r].tag == StructureTag::Figure ? figures : texts).push_back(r);
    }

    static const std::set<std::string> wrapperOps = {"q", "Q", "cm", "gs", "re", "W", "W*", "n"};
    // nonWrapper[i]: how many operators before i are not placement-wrapper
    // operators, so "does (qo, qc) hold anything else" is O(1).
    std::vector<int> nonWrapper(static_cast<std::size_t>(n) + 1, 0);
    for (int i = 0; i < n; ++i)
        nonWrapper[i + 1] = nonWrapper[i] + (wrapperOps.count(ops[i].keyword) ? 0 : 1);
    auto wrapperAround = [&](int k) {
        // The innermost q ... Q around op k, when it holds nothing but k and
        // placement-wrapper operators (PDFE's `q F cm [re W n] /Im Do Q`).
        const int qo = nest.qOpen[k];
        if (qo < 0 || nest.partner[qo] < 0) return std::make_pair(k, k);
        const int qc = nest.partner[qo];
        const int others = nonWrapper[qc] - nonWrapper[qo + 1] -
                           (wrapperOps.count(ops[k].keyword) ? 0 : 1);
        if (others != 0) return std::make_pair(k, k);
        return std::make_pair(qo, qc);
    };

    std::vector<std::pair<int, int>> images, atoms;
    for (int k = 0; k < n; ++k) {
        if (nest.mcDepth[k] != 0 || nest.inText[k]) continue;
        const std::string& kw = ops[k].keyword;
        if ((kw == "Do" && imageNames.count(ops[k].lastName)) || kw == "BI") {
            images.push_back(wrapperAround(k));
        } else if (kw == "Do" && formNames.count(ops[k].lastName)) {
            atoms.push_back(wrapperAround(k));
        } else if (kw == "BT" && nest.partner[k] >= 0) {
            atoms.emplace_back(k, nest.partner[k]);
        }
    }

    // Figures, one image draw each, in order.
    for (std::size_t i = 0; i < images.size() && i < figures.size(); ++i) {
        spans.push_back({images[i].first, images[i].second, figures[i]});
    }

    // Does [s, e] overlap an image draw? The draws sorted by start, with the
    // running maximum of their ends: O(log n) per question.
    std::vector<std::pair<int, int>> byStart(images);
    std::sort(byStart.begin(), byStart.end());
    std::vector<int> maxEnd(byStart.size());
    for (std::size_t i = 0; i < byStart.size(); ++i)
        maxEnd[i] = i ? std::max(byStart[i].second, maxEnd[i - 1]) : byStart[i].second;
    auto holdsImage = [&](int s, int e) {
        const auto upto = std::upper_bound(byStart.begin(), byStart.end(),
                                           std::make_pair(e, std::numeric_limits<int>::max()));
        if (upto == byStart.begin()) return false;
        return maxEnd[static_cast<std::size_t>(upto - byStart.begin()) - 1] >= s;
    };

    // Text atoms -> blocks: runs of atoms whose balanced hull holds no image.
    // The last block's hull is kept and grown, not recomputed from the
    // block's first atom: a hull is a closure (the hull of a span holding a
    // hull is the hull of the two together), so the result is the same, and
    // a block that starts deep in the nesting does not pay that depth again
    // for every atom it gains.
    std::vector<std::vector<std::pair<int, int>>> blocks;
    std::pair<int, int> blockHull{0, -1};
    for (const auto& a : atoms) {
        int s = a.first, e = a.second;
        if (!hulls.hull(s, e) || holdsImage(s, e)) {
            if (hulls.exhausted()) return {};
            continue;
        }
        if (!blocks.empty()) {
            int bs = blockHull.first, be = std::max(blockHull.second, e);
            if (hulls.hull(bs, be) && !holdsImage(bs, be)) {
                blocks.back().emplace_back(s, e);
                blockHull = {bs, be};
                continue;
            }
            if (hulls.exhausted()) return {};
        }
        blocks.push_back({{s, e}});
        blockHull = {s, e};
    }
    const std::size_t B = blocks.size(), N = texts.size();
    if (B == 0 || N == 0) return spans;

    std::vector<std::size_t> share(B, 1);
    if (N >= B) {
        // Every block gets one region; the rest go where atoms per region is
        // highest, never more regions than a block has atoms.
        // Highest ratio first, the lowest block among equals (what a linear
        // scan for the strict maximum picks), in O(log B) per region.
        std::size_t left = N - B;
        auto after = [](const std::pair<double, std::size_t>& a,
                        const std::pair<double, std::size_t>& b) {
            return a.first < b.first || (a.first == b.first && a.second > b.second);
        };
        std::priority_queue<std::pair<double, std::size_t>,
                            std::vector<std::pair<double, std::size_t>>, decltype(after)>
            next(after);
        for (std::size_t j = 0; j < B; ++j)
            if (share[j] < blocks[j].size())
                next.emplace(double(blocks[j].size()) / double(share[j]), j);
        while (left > 0 && !next.empty()) {
            const std::size_t best = next.top().second;
            next.pop();
            ++share[best];
            --left;
            if (share[best] < blocks[best].size())
                next.emplace(double(blocks[best].size()) / double(share[best]), best);
        }
    }

    std::size_t region = 0;
    for (std::size_t j = 0; j < B && region < N; ++j) {
        if (N < B && j * N / B != region) {
            // More text blocks than text regions: this block stays untagged.
            continue;
        }
        const auto& atomsOf = blocks[j];
        const std::size_t groups = std::min(share[j], atomsOf.size());
        int prevEnd = -1;
        for (std::size_t g = 0; g < groups && region < N; ++g) {
            const std::size_t a0 = g * atomsOf.size() / groups;
            const std::size_t a1 = (g + 1) * atomsOf.size() / groups - 1;
            int s = atomsOf[a0].first, e = atomsOf[a1].second;
            // A group whose hull would reach into the previous group's (the
            // nesting does not allow a cut between them) gets no sequence of
            // its own; its text stays in the previous group's sequence.
            if (hulls.hull(s, e) && s > prevEnd && !holdsImage(s, e)) {
                spans.push_back({s, e, texts[region]});
                prevEnd = e;
            } else if (hulls.exhausted()) {
                return {};
            } else if (!spans.empty() && spans.back().s <= s && e > spans.back().e) {
                int ms = spans.back().s, me = e;
                if (hulls.hull(ms, me) && ms == spans.back().s && !holdsImage(ms, me)) {
                    spans.back().e = me;
                    prevEnd = me;
                } else if (hulls.exhausted()) {
                    return {};
                }
            }
            ++region;
        }
    }
    return spans;
}

} // namespace

void ContentStreamTagger::tagPageContent(
    pdfe::core::Document* doc,
    int pageIndex,
    const std::vector<MarkedContentRegion>& regions)
{
    if (!doc || regions.empty()) return;

    // Everything with a destructor lives in THIS frame, not in the guarded
    // region (GitHub #118's hazard class): the MuPDF part only fills them.
    std::string existing;
    std::set<std::string> imageNames, formNames;
    bool loaded = false;

    impl_->context_->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return;
        if (pageIndex < 0 || pageIndex >= pdf_count_pages(ctx, pdfDoc)) return;
        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, pageIndex);
        if (!pageObj) return;
        pdf_obj* contents = pdf_dict_get(ctx, pageObj, PDF_NAME(Contents));
        if (!contents) return;

        fz_buffer* part = nullptr;
        fz_var(part);
        fz_try(ctx) {
            const int count = pdf_is_array(ctx, contents) ? pdf_array_len(ctx, contents) : 1;
            for (int i = 0; i < count; ++i) {
                pdf_obj* s = pdf_is_array(ctx, contents) ? pdf_array_get(ctx, contents, i)
                                                         : contents;
                if (!pdf_is_stream(ctx, s)) continue;
                part = pdf_load_stream(ctx, s);
                unsigned char* data = nullptr;
                const size_t len = fz_buffer_storage(ctx, part, &data);
                if (data) existing.append(reinterpret_cast<const char*>(data), len);
                existing.push_back('\n');
                fz_drop_buffer(ctx, part);
                part = nullptr;
            }
            pdf_obj* res = pdf_dict_get_inheritable(ctx, pageObj, PDF_NAME(Resources));
            pdf_obj* xo = pdf_dict_get(ctx, res, PDF_NAME(XObject));
            for (int i = 0, n = pdf_dict_len(ctx, xo); i < n; ++i) {
                pdf_obj* v = pdf_dict_get_val(ctx, xo, i);
                pdf_obj* st = pdf_dict_get(ctx, v, PDF_NAME(Subtype));
                const char* key = pdf_to_name(ctx, pdf_dict_get_key(ctx, xo, i));
                if (pdf_name_eq(ctx, st, PDF_NAME(Image))) imageNames.insert(key);
                else if (pdf_name_eq(ctx, st, PDF_NAME(Form))) formNames.insert(key);
            }
            loaded = true;
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, part);
        }
        fz_catch(ctx) {
            loaded = false;
        }
    });
    if (!loaded) return;

    // Which operators each region encloses.
    const std::vector<ContentOp> ops = lexOperators(existing);
    const Nesting nest = computeNesting(ops);
    HullIndex hulls(nest, hullBudgetFor(ops.size()));
    const int nOps = static_cast<int>(ops.size());
    bool anyRange = false;
    for (const auto& r : regions) anyRange = anyRange || r.endOperatorIndex > r.startOperatorIndex;

    std::vector<Span> spans;
    if (anyRange) {
        // Explicit ranges: [start, end) per region, in content order. A range
        // that would cut a q/Q, BT/ET or BDC/EMC pair in two is not written
        // (the stream would be malformed); that region is emitted empty.
        for (std::size_t r = 0; r < regions.size(); ++r) {
            const int s = std::max(0, regions[r].startOperatorIndex);
            const int e = std::min(nOps, regions[r].endOperatorIndex) - 1;
            if (e < s) continue;
            if (!hulls.isBalanced(s, e)) {
                qWarning("ContentStreamTagger: region MCID %d [%d, %d) cuts a nested "
                         "operator pair; left untagged", regions[r].mcid,
                         regions[r].startOperatorIndex, regions[r].endOperatorIndex);
                continue;
            }
            spans.push_back({s, e, r});
        }
        std::sort(spans.begin(), spans.end(),
                  [](const Span& a, const Span& b) { return a.s < b.s; });
        std::vector<Span> kept;
        for (const auto& sp : spans) {
            if (!kept.empty() && sp.s <= kept.back().e) {
                qWarning("ContentStreamTagger: region MCID %d overlaps an earlier region; "
                         "left untagged", regions[sp.region].mcid);
                continue;
            }
            kept.push_back(sp);
        }
        spans.swap(kept);
    } else {
        spans = autoPlace(ops, nest, hulls, regions, imageNames, formNames);
        if (hulls.exhausted()) {
            qWarning("ContentStreamTagger: page %d nests its operators too deeply to place "
                     "regions in; its content is left untagged", pageIndex);
        }
        std::sort(spans.begin(), spans.end(),
                  [](const Span& a, const Span& b) { return a.s < b.s; });
    }

    // A region tagged Artifact is page furniture: it gets "/Artifact ...
    // BDC" with NO /MCID, because an artifact is outside the structure tree and
    // must not occupy a ParentTree slot. Everything else gets the structure
    // type plus its /MCID.
    auto bdc = [](const MarkedContentRegion& region) {
        if (region.tag == StructureTag::Artifact) {
            return std::string("/Artifact <</Type /Pagination>> BDC\n");
        }
        return std::string("/") + structureTagToName(region.tag) + " <</MCID " +
               std::to_string(region.mcid) + ">> BDC\n";
    };

    std::string out;
    out.reserve(existing.size() + 64 * regions.size());
    std::vector<bool> placed(regions.size(), false);
    std::size_t cursor = 0;
    for (const auto& sp : spans) {
        const std::size_t from = ops[static_cast<std::size_t>(sp.s)].start;
        const std::size_t to = ops[static_cast<std::size_t>(sp.e)].end;
        out.append(existing, cursor, from - cursor);
        out += bdc(regions[sp.region]);
        out.append(existing, from, to - from);
        out += "\nEMC\n";
        cursor = to;
        placed[sp.region] = true;
    }
    out.append(existing, cursor, std::string::npos);
    // A region with nothing of its own on the page still gets its (empty)
    // sequence, so its MCID exists; it encloses nothing rather than the page.
    for (std::size_t r = 0; r < regions.size(); ++r) {
        if (placed[r]) continue;
        out += bdc(regions[r]);
        out += "EMC\n";
    }

    impl_->context_->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return;
        if (pageIndex < 0 || pageIndex >= pdf_count_pages(ctx, pdfDoc)) return;
        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, pageIndex);
        if (!pageObj) return;

        fz_buffer* newBuf = nullptr;
        pdf_obj* newStream = nullptr;
        fz_var(newBuf);
        fz_var(newStream);
        fz_try(ctx) {
            newBuf = fz_new_buffer_from_copied_data(
                ctx, reinterpret_cast<const unsigned char*>(out.data()), out.size());
            newStream = pdf_add_stream(ctx, pdfDoc, newBuf, nullptr, 0);
            pdf_dict_put(ctx, pageObj, PDF_NAME(Contents), newStream);
        }
        fz_always(ctx) {
            pdf_drop_obj(ctx, newStream);
            fz_drop_buffer(ctx, newBuf);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });
}

int ContentStreamTagger::markArtifacts(
    pdfe::core::Document* doc,
    int pageIndex,
    const std::vector<ArtifactRegion>& regions)
{
    if (!doc || regions.empty()) return 0;

    // BUILT IN THIS FRAME, NOT INSIDE THE LAMBDA (GitHub #118's hazard class).
    //
    // `os` and `added` used to be declared inside the executeReturn lambda,
    // whose whole body runs inside the fz_try in MuPdfContext. MuPDF's error
    // path is setjmp/longjmp, so an fz_throw from any of the pdf_* calls below
    // jumps to that fz_catch and skips every destructor in between — leaking
    // both the stringstream's buffer and the string's, which for a page with
    // real artifact content is well past the small-string optimisation.
    //
    // Nothing here needs `ctx`: artifactBdc is pure string formatting. So the
    // fix is not an out-parameter (as XmpMetadataWriter needed) but simply
    // hoisting the whole construction out, leaving the lambda with no
    // non-trivially-destructible local at all. `added` is then destroyed by
    // ordinary C++ unwinding after executeReturn returns, whatever MuPDF did.
    std::ostringstream os;
    for (const auto& r : regions) {
        // q/Q brackets each artifact so its graphics state cannot leak
        // into the page content it was appended after.
        os << "q\n" << artifactBdc(r);
        if (!r.contentOperators.empty()) {
            os << r.contentOperators;
            if (r.contentOperators.back() != '\n') os << '\n';
        }
        os << "EMC\nQ\n";
    }
    const std::string added = os.str();

    return impl_->context_->executeReturn<int>([&](fz_context* ctx) -> int {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return 0;

        int count = pdf_count_pages(ctx, pdfDoc);
        if (pageIndex < 0 || pageIndex >= count) return 0;

        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, pageIndex);
        if (!pageObj) return 0;

        int emitted = 0;
        fz_buffer* buf = nullptr;
        fz_var(buf);
        fz_try(ctx) {
            buf = fz_new_buffer_from_copied_data(
                ctx,
                reinterpret_cast<const unsigned char*>(added.data()),
                added.size());
            pdf_obj* newStream = pdf_add_stream(ctx, pdfDoc, buf, nullptr, 0);

            // APPEND rather than replace: the page's existing content (which
            // may already be marked) must survive untouched.
            pdf_obj* contents = pdf_dict_get(ctx, pageObj, PDF_NAME(Contents));
            if (contents && pdf_is_array(ctx, contents)) {
                pdf_array_push(ctx, contents, newStream);
            } else {
                pdf_obj* arr = pdf_new_array(ctx, pdfDoc, 2);
                if (contents) pdf_array_push(ctx, arr, contents);
                pdf_array_push(ctx, arr, newStream);
                pdf_dict_put(ctx, pageObj, PDF_NAME(Contents), arr);
                pdf_drop_obj(ctx, arr);
            }
            pdf_drop_obj(ctx, newStream);
            emitted = static_cast<int>(regions.size());
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, buf);
        }
        fz_catch(ctx) {
            emitted = 0;
        }
        return emitted;
    });
}

bool ContentStreamTagger::pageHasMarkedContent(pdfe::core::Document* doc, int pageIndex) const {
    if (!doc) return false;

    return impl_->context_->executeReturn<bool>([doc, pageIndex](fz_context* ctx) -> bool {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return false;

        int count = pdf_count_pages(ctx, pdfDoc);
        if (pageIndex < 0 || pageIndex >= count) return false;

        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, pageIndex);
        if (!pageObj) return false;

        pdf_obj* contents = pdf_dict_get(ctx, pageObj, PDF_NAME(Contents));
        if (!contents) return false;

        bool hasMarked = false;
        fz_buffer* buf = nullptr;

        fz_try(ctx) {
            if (pdf_is_array(ctx, contents)) {
                if (pdf_array_len(ctx, contents) > 0) {
                    buf = pdf_load_stream(ctx, pdf_array_get(ctx, contents, 0));
                }
            } else {
                buf = pdf_load_stream(ctx, contents);
            }

            if (buf) {
                unsigned char* data = nullptr;
                size_t len = fz_buffer_storage(ctx, buf, &data);
                // A view, not a std::string: nothing with a destructor may sit
                // inside the fz_try (GitHub #118 hazard class).
                const std::string_view content(reinterpret_cast<const char*>(data), data ? len : 0);
                hasMarked = content.find("BDC") != std::string_view::npos ||
                            content.find("BMC") != std::string_view::npos;
            }
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, buf);
        }
        fz_catch(ctx) {
            // Ignore errors
        }

        return hasMarked;
    });
}

} // namespace pdfe::compliance
