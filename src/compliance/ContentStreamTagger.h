#pragma once

#include "StructureTypes.h"
#include <memory>
#include <string>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::compliance {

struct MarkedContentRegion {
    StructureTag tag = StructureTag::P;
    int mcid = 0;
    int startOperatorIndex = 0;
    int endOperatorIndex = 0;
};

// ── Artifacts (PDF 32000-1 14.8.2.2) ──────────────────────────────────────
//
// Running heads, folios, rules and watermarks are page furniture, not
// document content. They must be emitted inside an
// "/Artifact <<...>> BDC ... EMC" sequence so a reader skips them, and they
// must NOT carry an /MCID or appear in the structure tree or ParentTree.
enum class ArtifactType {
    Pagination,   // running head / footer / folio
    Layout,       // rules, boxes, decorative separators
    Page,         // cut marks, colour bars
    Background    // watermarks, backdrops
};

enum class ArtifactSubtype {
    None,
    Header,
    Footer,
    Watermark
};

struct ArtifactRegion {
    ArtifactType type = ArtifactType::Pagination;
    ArtifactSubtype subtype = ArtifactSubtype::None;
    bool hasBBox = false;
    float bbox[4] = {0.f, 0.f, 0.f, 0.f};   // x0 y0 x1 y1, default user space
    std::string contentOperators;           // e.g. "BT /F1 9 Tf 72 40 Td (3) Tj ET"
};

const char* artifactTypeToName(ArtifactType type);
const char* artifactSubtypeToName(ArtifactSubtype subtype);   // nullptr for None

class ContentStreamTagger {
public:
    explicit ContentStreamTagger(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~ContentStreamTagger();
    ContentStreamTagger(const ContentStreamTagger&) = delete;
    ContentStreamTagger& operator=(const ContentStreamTagger&) = delete;

    void tagPageContent(
        pdfe::core::Document* doc,
        int pageIndex,
        const std::vector<MarkedContentRegion>& regions
    );

    bool pageHasMarkedContent(pdfe::core::Document* doc, int pageIndex) const;

    /// Emit `regions` into the page's content as artifact-marked sequences,
    /// APPENDED as an additional content stream so existing (possibly already
    /// marked) content is left byte-for-byte alone. Each region becomes
    ///
    ///     q
    ///     /Artifact <</Type /Pagination /Subtype /Header /BBox [...]>> BDC
    ///     <contentOperators>
    ///     EMC
    ///     Q
    ///
    /// No /MCID is written: an artifact is outside the structure tree by
    /// definition. Returns the number of regions emitted (0 if the document or
    /// page index is unusable), so a caller cannot mistake "did nothing" for
    /// success.
    int markArtifacts(
        pdfe::core::Document* doc,
        int pageIndex,
        const std::vector<ArtifactRegion>& regions
    );

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::compliance
