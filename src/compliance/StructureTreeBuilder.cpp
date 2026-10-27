#include "StructureTreeBuilder.h"
#include "../core/MuPdfContext.h"
#include "../core/Document.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace pdfe::compliance {

class StructureTreeBuilder::Impl {
public:
    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> context)
        : context_(std::move(context))
    {}

    std::shared_ptr<pdfe::core::MuPdfContext> context_;

    // Custom structure type -> standard type it rolls up to, accumulated while
    // the tree is built and written out as /StructTreeRoot /RoleMap. Reset at
    // the top of every buildTree() call so a second build cannot inherit the
    // first one's entries.
    std::map<std::string, std::string> roleMap_;

    // An ARTIFACT is not a structure element. Pagination/layout content is
    // marked in the content stream as "/Artifact ... BDC ... EMC" and must be
    // excluded from the structure tree and from the ParentTree entirely; a
    // /StructElem with /S /Artifact is not a thing the spec defines.
    static bool isArtifact(const StructureNode& node) {
        return node.tag == StructureTag::Artifact;
    }

    // Recursive: build StructElem objects for a node and its children
    pdf_obj* buildStructElem(
        fz_context* ctx,
        pdf_document* pdfDoc,
        const StructureNode& node,
        pdf_obj* parentRef,
        std::vector<std::pair<int, pdf_obj*>>& parentTreeEntries)
    {
        pdf_obj* elem = pdf_new_dict(ctx, pdfDoc, 6);
        pdf_obj* elemRef = pdf_add_object(ctx, pdfDoc, elem);
        pdf_drop_obj(ctx, elem);
        elem = elemRef;

        pdf_dict_puts(ctx, elem, "Type", pdf_new_name(ctx, "StructElem"));

        // A custom (non-standard) type is written verbatim as /S and recorded
        // in the role map against the standard type in `node.tag`. Without the
        // /RoleMap entry the element is meaningless to an assistive-technology
        // consumer and the document fails accessibility validation.
        const char* standardName = structureTagToName(node.tag);
        if (!node.customTag.empty()) {
            pdf_dict_put_name(ctx, elem, PDF_NAME(S), node.customTag.c_str());
            roleMap_[node.customTag] = standardName;
        } else {
            pdf_dict_put_name(ctx, elem, PDF_NAME(S), standardName);
        }
        pdf_dict_put(ctx, elem, PDF_NAME(P), parentRef);

        // If leaf node (has MCID), set /K and /Pg
        if (node.mcid >= 0 && node.children.empty()) {
            // /K is the MCID integer for leaf
            pdf_dict_put_int(ctx, elem, PDF_NAME(K), node.mcid);

            // /Pg references the page
            if (node.pageIndex >= 0) {
                pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, node.pageIndex);
                if (pageObj) {
                    pdf_dict_puts(ctx, elem, "Pg", pageObj);
                }
            }

            // Record for parent tree
            parentTreeEntries.push_back({node.mcid, elem});
        }

        // If has alt text, set /Alt
        if (!node.altText.empty()) {
            pdf_dict_put_text_string(ctx, elem, PDF_NAME(Alt), node.altText.c_str());
        }

        // Build children (artifacts are content-stream marks, not elements)
        if (!node.children.empty()) {
            pdf_obj* kids = pdf_new_array(ctx, pdfDoc, static_cast<int>(node.children.size()));
            for (const auto& child : node.children) {
                if (!child || isArtifact(*child)) continue;
                pdf_obj* childElem = buildStructElem(ctx, pdfDoc, *child, elem, parentTreeEntries);
                pdf_array_push(ctx, kids, childElem);
                pdf_drop_obj(ctx, childElem);
            }
            if (pdf_array_len(ctx, kids) > 0) {
                pdf_dict_put(ctx, elem, PDF_NAME(K), kids);
            }
            pdf_drop_obj(ctx, kids);
        }

        return elem;
    }
};

StructureTreeBuilder::StructureTreeBuilder(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{}

StructureTreeBuilder::~StructureTreeBuilder() = default;

void StructureTreeBuilder::buildTree(pdfe::core::Document* doc, const StructureNode& root) {
    if (!doc) return;

    impl_->context_->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return;

        pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
        if (!catalog) return;

        impl_->roleMap_.clear();

        // Carry over any /RoleMap the document already declared. buildTree
        // REPLACES /StructTreeRoot wholesale, so without this a rebuild silently
        // drops the mapping for every custom type the previous tree declared and
        // the surviving custom /S names become meaningless. Entries for types
        // that no longer occur are harmless (the spec permits a superset).
        {
            pdf_obj* oldRoot = pdf_dict_get(ctx, catalog, PDF_NAME(StructTreeRoot));
            pdf_obj* oldRoleMap = oldRoot ? pdf_dict_gets(ctx, oldRoot, "RoleMap") : nullptr;
            if (oldRoleMap && pdf_is_dict(ctx, oldRoleMap)) {
                int n = pdf_dict_len(ctx, oldRoleMap);
                for (int i = 0; i < n; ++i) {
                    const char* key = pdf_to_name(ctx, pdf_dict_get_key(ctx, oldRoleMap, i));
                    const char* val = pdf_to_name(ctx, pdf_dict_get_val(ctx, oldRoleMap, i));
                    if (key && *key && val && *val) impl_->roleMap_[key] = val;
                }
            }
        }

        // Create StructTreeRoot
        pdf_obj* treeRoot = pdf_new_dict(ctx, pdfDoc, 4);
        pdf_obj* treeRootRef = pdf_add_object(ctx, pdfDoc, treeRoot);
        pdf_drop_obj(ctx, treeRoot);
        treeRoot = treeRootRef;

        pdf_dict_puts(ctx, treeRoot, "Type", pdf_new_name(ctx, "StructTreeRoot"));

        // Build all StructElem nodes
        std::vector<std::pair<int, pdf_obj*>> parentTreeEntries;
        pdf_obj* kids = pdf_new_array(ctx, pdfDoc, static_cast<int>(root.children.size()));

        for (const auto& child : root.children) {
            if (!child || Impl::isArtifact(*child)) continue;
            pdf_obj* childElem = impl_->buildStructElem(
                ctx, pdfDoc, *child, treeRoot, parentTreeEntries);
            pdf_array_push(ctx, kids, childElem);
            pdf_drop_obj(ctx, childElem);
        }

        pdf_dict_put(ctx, treeRoot, PDF_NAME(K), kids);
        pdf_drop_obj(ctx, kids);

        // /RoleMap: every custom /S emitted above, mapped to the standard type
        // it rolls up to. Written only when there is something to map — an
        // empty /RoleMap dictionary is noise, not information.
        if (!impl_->roleMap_.empty()) {
            pdf_obj* roleMapObj = pdf_new_dict(ctx, pdfDoc,
                static_cast<int>(impl_->roleMap_.size()));
            for (const auto& [custom, standard] : impl_->roleMap_) {
                pdf_dict_puts_drop(ctx, roleMapObj, custom.c_str(),
                                   pdf_new_name(ctx, standard.c_str()));
            }
            pdf_dict_puts(ctx, treeRoot, "RoleMap", roleMapObj);
            pdf_drop_obj(ctx, roleMapObj);
        }

        // Build ParentTree (number tree)
        if (!parentTreeEntries.empty()) {
            pdf_obj* parentTree = pdf_new_dict(ctx, pdfDoc, 2);
            pdf_obj* numsArray = pdf_new_array(ctx, pdfDoc,
                static_cast<int>(parentTreeEntries.size() * 2));

            // Sort by MCID
            std::sort(parentTreeEntries.begin(), parentTreeEntries.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            for (const auto& entry : parentTreeEntries) {
                pdf_array_push_int(ctx, numsArray, entry.first);
                pdf_array_push(ctx, numsArray, entry.second);
            }

            pdf_dict_put(ctx, parentTree, PDF_NAME(Nums), numsArray);
            pdf_drop_obj(ctx, numsArray);

            // parentTreeEntries is non-empty (outer guard at 117).
            int nextKey = parentTreeEntries.back().first + 1;
            pdf_dict_puts(ctx, parentTree, "ParentTreeNextKey",
                pdf_new_int(ctx, nextKey));

            pdf_obj* parentTreeRef = pdf_add_object(ctx, pdfDoc, parentTree);
            pdf_drop_obj(ctx, parentTree);
            pdf_dict_put(ctx, treeRoot, PDF_NAME(ParentTree), parentTreeRef);
            pdf_drop_obj(ctx, parentTreeRef);
        }

        // Set StructTreeRoot on catalog
        pdf_dict_put(ctx, catalog, PDF_NAME(StructTreeRoot), treeRoot);

        // Set /MarkInfo /Marked true
        pdf_obj* markInfo = pdf_dict_gets(ctx, catalog, "MarkInfo");
        if (!markInfo) {
            markInfo = pdf_new_dict(ctx, pdfDoc, 1);
            pdf_dict_puts(ctx, catalog, "MarkInfo", markInfo);
            pdf_drop_obj(ctx, markInfo);
        }
        pdf_dict_puts(ctx, markInfo, "Marked", PDF_TRUE);

        // Set StructParents on pages that have marked content
        // We need to check which pages have leaf StructElem nodes referencing them.
        // The parentTreeEntries contain (mcid, elemRef) pairs; each elemRef has a /Pg key.
        int pageCount = pdf_count_pages(ctx, pdfDoc);
        for (int i = 0; i < pageCount; ++i) {
            pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, i);
            if (!pageObj) continue;

            bool pageHasContent = false;
            for (const auto& entry : parentTreeEntries) {
                pdf_obj* elemPg = pdf_dict_gets(ctx, entry.second, "Pg");
                if (elemPg && pdf_objcmp(ctx, elemPg, pageObj) == 0) {
                    pageHasContent = true;
                    break;
                }
            }
            if (pageHasContent) {
                pdf_dict_put_int(ctx, pageObj, PDF_NAME(StructParents), i);
            }
        }

        pdf_drop_obj(ctx, treeRoot);
    });
}

bool StructureTreeBuilder::hasStructureTree(pdfe::core::Document* doc) const {
    if (!doc) return false;

    return impl_->context_->executeReturn<bool>([doc](fz_context* ctx) -> bool {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return false;

        pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
        if (!catalog) return false;

        const pdf_obj* structTree = pdf_dict_gets(ctx, catalog, "StructTreeRoot");
        return structTree != nullptr;
    });
}

void StructureTreeBuilder::removeStructureTree(pdfe::core::Document* doc) {
    if (!doc) return;

    impl_->context_->execute([doc](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return;

        pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
        if (!catalog) return;

        pdf_dict_dels(ctx, catalog, "StructTreeRoot");
        pdf_dict_dels(ctx, catalog, "MarkInfo");
    });
}

void StructureTreeBuilder::setDocumentLanguage(pdfe::core::Document* doc, const std::string& lang) {
    if (!doc || lang.empty()) return;

    impl_->context_->execute([doc, &lang](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return;

        pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
        if (!catalog) return;

        pdf_dict_put_text_string(ctx, catalog, PDF_NAME(Lang), lang.c_str());
    });
}

} // namespace pdfe::compliance
