#pragma once

#include <string>
#include <vector>
#include <memory>

namespace pdfe::compliance {

enum class StructureTag {
    Document,
    Part,
    Sect,
    H1, H2, H3, H4, H5, H6,
    P,
    Span,
    Figure,
    Table, TR, TH, TD,
    L, LI, Lbl, LBody,
    Link,
    Artifact
};

struct StructureNode {
    StructureTag tag = StructureTag::P;

    // Non-standard ("custom") structure type. When non-empty the element's /S
    // is emitted as this name and `tag` becomes the STANDARD type it rolls up
    // to, which StructureTreeBuilder records in /StructTreeRoot /RoleMap.
    //
    // A custom /S with no /RoleMap entry is exactly what makes a tagged PDF
    // fail accessibility validation: a consumer has no way to know what the
    // element means. Leave empty for a standard tag.
    std::string customTag;

    std::string altText;

    // Set by AutoTagger on a node that REQUIRES author-supplied alternative
    // text (a /Figure) but for which none could be derived. Deliberately not
    // satisfied with a fabricated placeholder — a generic "Image" string
    // silences an automated checker while leaving the document unusable for a
    // screen-reader user, which is worse than an honest, visible omission.
    bool needsAltText = false;

    int pageIndex = -1;
    int mcid = -1;           // Marked Content ID (-1 for non-leaf nodes)
    std::vector<std::shared_ptr<StructureNode>> children;
};

inline const char* structureTagToName(StructureTag tag) {
    switch (tag) {
        case StructureTag::Document: return "Document";
        case StructureTag::Part: return "Part";
        case StructureTag::Sect: return "Sect";
        case StructureTag::H1: return "H1";
        case StructureTag::H2: return "H2";
        case StructureTag::H3: return "H3";
        case StructureTag::H4: return "H4";
        case StructureTag::H5: return "H5";
        case StructureTag::H6: return "H6";
        case StructureTag::P: return "P";
        case StructureTag::Span: return "Span";
        case StructureTag::Figure: return "Figure";
        case StructureTag::Table: return "Table";
        case StructureTag::TR: return "TR";
        case StructureTag::TH: return "TH";
        case StructureTag::TD: return "TD";
        case StructureTag::L: return "L";
        case StructureTag::LI: return "LI";
        case StructureTag::Lbl: return "Lbl";
        case StructureTag::LBody: return "LBody";
        case StructureTag::Link: return "Link";
        case StructureTag::Artifact: return "Artifact";
        default: return "Span";
    }
}

inline StructureTag nameToStructureTag(const char* name) {
    if (!name) return StructureTag::P;
    std::string s(name);
    if (s == "Document") return StructureTag::Document;
    if (s == "Part") return StructureTag::Part;
    if (s == "Sect") return StructureTag::Sect;
    if (s == "H1") return StructureTag::H1;
    if (s == "H2") return StructureTag::H2;
    if (s == "H3") return StructureTag::H3;
    if (s == "H4") return StructureTag::H4;
    if (s == "H5") return StructureTag::H5;
    if (s == "H6") return StructureTag::H6;
    if (s == "P") return StructureTag::P;
    if (s == "Span") return StructureTag::Span;
    if (s == "Figure") return StructureTag::Figure;
    if (s == "Table") return StructureTag::Table;
    if (s == "TR") return StructureTag::TR;
    if (s == "TH") return StructureTag::TH;
    if (s == "TD") return StructureTag::TD;
    if (s == "L") return StructureTag::L;
    if (s == "LI") return StructureTag::LI;
    if (s == "Lbl") return StructureTag::Lbl;
    if (s == "LBody") return StructureTag::LBody;
    if (s == "Link") return StructureTag::Link;
    if (s == "Artifact") return StructureTag::Artifact;
    return StructureTag::P;
}

} // namespace pdfe::compliance
