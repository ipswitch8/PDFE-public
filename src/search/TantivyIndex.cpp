#include "TantivyIndex.h"
#include "TantivyBridge.h"

#include <stdexcept>
#include <string>

namespace pdfe::search {

struct TantivyIndex::Impl {
    TantivyIndexHandle* handle = nullptr;
};

TantivyIndex::TantivyIndex(const std::filesystem::path& indexDir)
    : impl_(std::make_unique<Impl>())
{
    const std::string s = indexDir.string();
    impl_->handle = pdfe_tantivy_open(s.c_str());
    if (!impl_->handle) {
        throw std::runtime_error(
            "TantivyIndex: failed to open index at " + s);
    }
}

TantivyIndex::~TantivyIndex()
{
    if (impl_ && impl_->handle) {
        pdfe_tantivy_close(impl_->handle);
        impl_->handle = nullptr;
    }
}

TantivyIndex::TantivyIndex(TantivyIndex&& other) noexcept
    : impl_(std::move(other.impl_))
{
}

TantivyIndex& TantivyIndex::operator=(TantivyIndex&& other) noexcept
{
    if (this != &other) {
        if (impl_ && impl_->handle) {
            pdfe_tantivy_close(impl_->handle);
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

bool TantivyIndex::valid() const
{
    return impl_ && impl_->handle != nullptr;
}

bool TantivyIndex::upsertPage(int pageIndex, const std::string& text)
{
    if (!valid()) return false;
    // Length-taking, NOT c_str(): page text comes from the document, and a PDF
    // may map a glyph to U+0000. c_str() would stop there and silently drop the
    // rest of the page out of the index — the document choosing what is
    // findable, with no error to notice.
    return pdfe_tantivy_upsert_page_len(impl_->handle, pageIndex,
                                        text.data(), text.size()) == 0;
}

bool TantivyIndex::deletePage(int pageIndex)
{
    if (!valid()) return false;
    return pdfe_tantivy_delete_page(impl_->handle, pageIndex) == 0;
}

bool TantivyIndex::commit()
{
    if (!valid()) return false;
    return pdfe_tantivy_commit(impl_->handle) == 0;
}

std::int64_t TantivyIndex::documentCount() const
{
    if (!valid()) return 0;
    return pdfe_tantivy_num_docs(impl_->handle);
}

std::vector<SearchHit> TantivyIndex::search(const std::string& query,
                                            int maxHits) const
{
    std::vector<SearchHit> out;
    if (!valid()) return out;

    TantivyResults* r = pdfe_tantivy_search(impl_->handle, query.c_str(), maxHits);
    if (!r) return out;
    out.reserve(static_cast<size_t>(r->count));
    for (int i = 0; i < r->count; ++i) {
        const TantivyHit& h = r->hits[i];
        SearchHit sh;
        sh.pageIndex = h.page_idx;
        sh.score     = h.score;
        sh.snippet   = h.snippet ? h.snippet : "";
        out.push_back(std::move(sh));
    }
    pdfe_tantivy_free_results(r);
    return out;
}

} // namespace pdfe::search
