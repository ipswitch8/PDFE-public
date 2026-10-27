#pragma once

#include "StructureTypes.h"
#include "PageFurnitureDetector.h"
#include <memory>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::compliance {

class AutoTagger {
public:
    explicit AutoTagger(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~AutoTagger();
    AutoTagger(const AutoTagger&) = delete;
    AutoTagger& operator=(const AutoTagger&) = delete;

    /// GitHub #173 — which blocks on which pages are page furniture.
    ///
    /// Detection lives on AutoTagger rather than in a class of its own for two
    /// reasons: ordering (its result has to exist before tagPage starts handing
    /// out MCIDs, and a method on the tagger makes that sequence hard to get
    /// wrong), and access (walking the pages needs Document's fz handle, which
    /// only Document's declared friends may take — AutoTagger is one, and
    /// adding another would mean editing Document.h, a header 27+ translation
    /// units include).
    std::vector<FurnitureRegion> detectPageFurniture(
        pdfe::core::Document* doc,
        const FurnitureOptions& options = {}
    );

    /// GitHub #173 — rotated, centred watermarks.
    ///
    /// A SEPARATE signal from detectPageFurniture, not a widening of it: see
    /// WatermarkOptions for why the band-plus-repeat test structurally cannot
    /// find a watermark. The output is the same FurnitureRegion vector and is
    /// meant to be concatenated with detectPageFurniture's before being handed
    /// to tagDocument / tagPage, so everything downstream stays unchanged.
    std::vector<FurnitureRegion> detectWatermarks(
        pdfe::core::Document* doc,
        const WatermarkOptions& options = {}
    );

    // `furniture` below is that detection's output. A block
    // that falls inside one of those regions is tagged StructureTag::Artifact
    // and is NOT given an MCID — an artifact has no place in the structure tree
    // or the ParentTree, and handing it one is exactly what gets a running head
    // read aloud as body text.
    //
    // Taking it as a PARAMETER rather than detecting here is the fix for the
    // ordering trap #173 names: detection has to have happened before the MCID
    // counter starts moving, because a post-pass would have to unpick ids.
    // Default-empty, so every pre-existing caller keeps its current behaviour.
    StructureNode tagDocument(
        pdfe::core::Document* doc,
        const std::vector<FurnitureRegion>& furniture = {}
    );
    std::vector<StructureNode> tagPage(
        pdfe::core::Document* doc,
        int pageIndex,
        int& nextMcid,
        const std::vector<FurnitureRegion>& furniture = {}
    );

private:
    // Impl is defined in AutoTagger.cpp; detectPageFurniture lives in
    // PageFurnitureDetector.cpp and reaches the context through this accessor
    // rather than through Impl's private layout.
    std::shared_ptr<pdfe::core::MuPdfContext> context() const;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::compliance
