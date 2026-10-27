#include "LayerManager.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <stdexcept>

namespace pdfe::layers {

class LayerManager::Impl {
public:
    std::shared_ptr<pdfe::core::MuPdfContext> context;

    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
        : context(std::move(ctx)) {}

    pdf_document* pdfDoc(const pdfe::core::Document* doc) {
        if (!doc) return nullptr;
        pdf_document* result = nullptr;
        context->execute([&](fz_context* ctx) {
            result = pdf_document_from_fz_document(ctx, doc->fzDocument());
        });
        return result;
    }
};

LayerManager::LayerManager(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{
}

LayerManager::~LayerManager() = default;

bool LayerManager::hasLayers(pdfe::core::Document* doc) {
    if (!doc) return false;
    return layerCount(doc) > 0;
}

// Helper: read /OCProperties/OCGs array directly, bypassing MuPDF's cached descriptor
// which can be stale after mutations
static pdf_obj* getOcgsArray(fz_context* ctx, pdf_document* pdoc) {
    if (!pdoc) return nullptr;
    pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, pdoc), PDF_NAME(Root));
    if (!root) return nullptr;
    pdf_obj* ocProps = pdf_dict_get(ctx, root, PDF_NAME(OCProperties));
    if (!ocProps) return nullptr;
    pdf_obj* ocgs = pdf_dict_get(ctx, ocProps, PDF_NAME(OCGs));
    if (!pdf_is_array(ctx, ocgs)) return nullptr;
    return ocgs;
}

int LayerManager::layerCount(pdfe::core::Document* doc) {
    if (!doc) return 0;
    int count = 0;
    impl_->context->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        pdf_obj* ocgs = getOcgsArray(ctx, pdoc);
        if (ocgs) count = pdf_array_len(ctx, ocgs);
    });
    return count;
}

std::vector<LayerInfo> LayerManager::listLayers(pdfe::core::Document* doc) {
    std::vector<LayerInfo> result;
    if (!doc) return result;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        pdf_obj* ocgs = getOcgsArray(ctx, pdoc);
        if (!ocgs) return;

        int count = pdf_array_len(ctx, ocgs);
        for (int i = 0; i < count; ++i) {
            pdf_obj* ocg = pdf_array_get(ctx, ocgs, i);
            LayerInfo info;
            info.index = i;
            pdf_obj* nameObj = pdf_dict_get(ctx, ocg, PDF_NAME(Name));
            if (nameObj) {
                const char* n = pdf_to_text_string(ctx, nameObj);
                info.name = n ? n : "";
            }
            // Visibility from MuPDF's layer API (which uses /D/OFF list)
            if (i < pdf_count_layers(ctx, pdoc)) {
                info.visible = pdf_layer_is_enabled(ctx, pdoc, i) != 0;
            } else {
                info.visible = true;
            }
            info.locked = false;
            result.push_back(info);
        }
    });

    return result;
}

bool LayerManager::isVisible(pdfe::core::Document* doc, int layerIndex) {
    if (!doc) return false;
    bool result = false;
    impl_->context->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (pdoc && layerIndex >= 0 && layerIndex < pdf_count_layers(ctx, pdoc)) {
            result = pdf_layer_is_enabled(ctx, pdoc, layerIndex) != 0;
        }
    });
    return result;
}

std::string LayerManager::layerName(pdfe::core::Document* doc, int layerIndex) {
    if (!doc) return "";
    std::string result;
    impl_->context->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        pdf_obj* ocgs = getOcgsArray(ctx, pdoc);
        if (ocgs && layerIndex >= 0 && layerIndex < pdf_array_len(ctx, ocgs)) {
            pdf_obj* ocg = pdf_array_get(ctx, ocgs, layerIndex);
            pdf_obj* nameObj = pdf_dict_get(ctx, ocg, PDF_NAME(Name));
            if (nameObj) {
                const char* n = pdf_to_text_string(ctx, nameObj);
                if (n) result = n;
            }
        }
    });
    return result;
}

void LayerManager::setVisible(pdfe::core::Document* doc, int layerIndex, bool visible) {
    if (!doc) return;
    impl_->context->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (pdoc && layerIndex >= 0 && layerIndex < pdf_count_layers(ctx, pdoc)) {
            pdf_enable_layer(ctx, pdoc, layerIndex, visible ? 1 : 0);
        }
    });
}

int LayerManager::createLayer(pdfe::core::Document* doc, const std::string& name) {
    if (!doc || name.empty()) return -1;

    int newIndex = -1;
    impl_->context->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdoc) return;

        // Get or create /Root/OCProperties
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, pdoc), PDF_NAME(Root));
        if (!root) return;

        pdf_obj* ocProps = pdf_dict_get(ctx, root, PDF_NAME(OCProperties));
        if (!ocProps) {
            ocProps = pdf_new_dict(ctx, pdoc, 2);
            pdf_dict_put_drop(ctx, root, PDF_NAME(OCProperties), ocProps);
        }

        // Get or create /OCGs array
        pdf_obj* ocgs = pdf_dict_get(ctx, ocProps, PDF_NAME(OCGs));
        if (!ocgs) {
            ocgs = pdf_new_array(ctx, pdoc, 4);
            pdf_dict_put_drop(ctx, ocProps, PDF_NAME(OCGs), ocgs);
        }

        // Create new OCG dict
        pdf_obj* ocg = pdf_new_dict(ctx, pdoc, 3);
        pdf_dict_put(ctx, ocg, PDF_NAME(Type), PDF_NAME(OCG));
        pdf_dict_put_text_string(ctx, ocg, PDF_NAME(Name), name.c_str());

        // Add as indirect reference
        pdf_obj* indRef = pdf_add_object(ctx, pdoc, ocg);
        pdf_drop_obj(ctx, ocg);
        pdf_array_push(ctx, ocgs, indRef);

        // Also add to /D/Order array for visibility
        pdf_obj* defaultConfig = pdf_dict_get(ctx, ocProps, PDF_NAME(D));
        if (!defaultConfig) {
            defaultConfig = pdf_new_dict(ctx, pdoc, 2);
            pdf_dict_put_drop(ctx, ocProps, PDF_NAME(D), defaultConfig);
        }

        pdf_obj* order = pdf_dict_get(ctx, defaultConfig, PDF_NAME(Order));
        if (!order) {
            order = pdf_new_array(ctx, pdoc, 4);
            pdf_dict_put_drop(ctx, defaultConfig, PDF_NAME(Order), order);
        }
        pdf_array_push(ctx, order, indRef);
        pdf_drop_obj(ctx, indRef);

        // New layer is last in /OCGs array
        newIndex = pdf_array_len(ctx, ocgs) - 1;
    });

    return newIndex;
}

void LayerManager::renameLayer(pdfe::core::Document* doc, int layerIndex, const std::string& newName) {
    if (!doc || newName.empty()) return;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdoc) return;

        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, pdoc), PDF_NAME(Root));
        if (!root) return;

        pdf_obj* ocProps = pdf_dict_get(ctx, root, PDF_NAME(OCProperties));
        if (!ocProps) return;

        pdf_obj* ocgs = pdf_dict_get(ctx, ocProps, PDF_NAME(OCGs));
        if (!ocgs || !pdf_is_array(ctx, ocgs)) return;

        if (layerIndex < 0 || layerIndex >= pdf_array_len(ctx, ocgs)) return;

        pdf_obj* ocg = pdf_array_get(ctx, ocgs, layerIndex);
        if (ocg) {
            pdf_dict_put_text_string(ctx, ocg, PDF_NAME(Name), newName.c_str());
        }
    });
}

void LayerManager::deleteLayer(pdfe::core::Document* doc, int layerIndex) {
    if (!doc) return;
    // Deleting a layer orphans its OCG object; force a full-rewrite save so
    // it is scrubbed (pipeline b2e47d91, Tier 1). createLayer/renameLayer/
    // reorderLayer are additive or in-place and stay incremental-safe.
    doc->markRequiresFullRewrite();

    impl_->context->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdoc) return;

        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, pdoc), PDF_NAME(Root));
        if (!root) return;

        pdf_obj* ocProps = pdf_dict_get(ctx, root, PDF_NAME(OCProperties));
        if (!ocProps) return;

        pdf_obj* ocgs = pdf_dict_get(ctx, ocProps, PDF_NAME(OCGs));
        if (!ocgs || !pdf_is_array(ctx, ocgs)) return;

        if (layerIndex < 0 || layerIndex >= pdf_array_len(ctx, ocgs)) return;

        // Capture the layer reference before deletion
        pdf_obj* removedRef = pdf_array_get(ctx, ocgs, layerIndex);

        // Remove from /OCGs
        pdf_array_delete(ctx, ocgs, layerIndex);

        // Remove from /D/Order if present
        pdf_obj* defaultConfig = pdf_dict_get(ctx, ocProps, PDF_NAME(D));
        if (defaultConfig) {
            pdf_obj* order = pdf_dict_get(ctx, defaultConfig, PDF_NAME(Order));
            if (order && pdf_is_array(ctx, order)) {
                int orderLen = pdf_array_len(ctx, order);
                for (int i = orderLen - 1; i >= 0; --i) {
                    pdf_obj* entry = pdf_array_get(ctx, order, i);
                    if (pdf_objcmp(ctx, entry, removedRef) == 0) {
                        pdf_array_delete(ctx, order, i);
                    }
                }
            }
        }

    });
}

void LayerManager::reorderLayer(pdfe::core::Document* doc, int fromIndex, int toIndex) {
    if (!doc || fromIndex == toIndex) return;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdoc) return;

        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, pdoc), PDF_NAME(Root));
        if (!root) return;

        pdf_obj* ocProps = pdf_dict_get(ctx, root, PDF_NAME(OCProperties));
        if (!ocProps) return;

        pdf_obj* ocgs = pdf_dict_get(ctx, ocProps, PDF_NAME(OCGs));
        if (!ocgs || !pdf_is_array(ctx, ocgs)) return;

        int len = pdf_array_len(ctx, ocgs);
        if (fromIndex < 0 || fromIndex >= len || toIndex < 0 || toIndex >= len) return;

        // Swap-based reorder: grab item, delete, insert
        pdf_obj* item = pdf_keep_obj(ctx, pdf_array_get(ctx, ocgs, fromIndex));
        pdf_array_delete(ctx, ocgs, fromIndex);
        pdf_array_insert(ctx, ocgs, item, toIndex);
        pdf_drop_obj(ctx, item);
    });
}

// ===== Commands =====

SetLayerVisibilityCommand::SetLayerVisibilityCommand(
    LayerManager* mgr, pdfe::core::Document* doc, int layerIndex, bool visible)
    : mgr_(mgr), doc_(doc), layerIndex_(layerIndex), newVisible_(visible)
{
    oldVisible_ = mgr_->isVisible(doc_, layerIndex_);
}

void SetLayerVisibilityCommand::execute() {
    mgr_->setVisible(doc_, layerIndex_, newVisible_);
}

void SetLayerVisibilityCommand::undo() {
    mgr_->setVisible(doc_, layerIndex_, oldVisible_);
}

std::string SetLayerVisibilityCommand::description() const {
    return newVisible_ ? "Show Layer" : "Hide Layer";
}

CreateLayerCommand::CreateLayerCommand(
    LayerManager* mgr, pdfe::core::Document* doc, const std::string& name)
    : mgr_(mgr), doc_(doc), name_(name)
{
}

void CreateLayerCommand::execute() {
    createdIndex_ = mgr_->createLayer(doc_, name_);
}

void CreateLayerCommand::undo() {
    if (createdIndex_ >= 0) {
        mgr_->deleteLayer(doc_, createdIndex_);
        createdIndex_ = -1;
    }
}

std::string CreateLayerCommand::description() const {
    return "Create Layer '" + name_ + "'";
}

RenameLayerCommand::RenameLayerCommand(
    LayerManager* mgr, pdfe::core::Document* doc, int layerIndex, const std::string& newName)
    : mgr_(mgr), doc_(doc), layerIndex_(layerIndex), newName_(newName)
{
    oldName_ = mgr_->layerName(doc_, layerIndex_);
}

void RenameLayerCommand::execute() {
    mgr_->renameLayer(doc_, layerIndex_, newName_);
}

void RenameLayerCommand::undo() {
    mgr_->renameLayer(doc_, layerIndex_, oldName_);
}

std::string RenameLayerCommand::description() const {
    return "Rename Layer";
}

DeleteLayerCommand::DeleteLayerCommand(
    LayerManager* mgr, pdfe::core::Document* doc, int layerIndex)
    : mgr_(mgr), doc_(doc), layerIndex_(layerIndex)
{
    deletedName_ = mgr_->layerName(doc_, layerIndex_);
    deletedVisible_ = mgr_->isVisible(doc_, layerIndex_);
}

void DeleteLayerCommand::execute() {
    mgr_->deleteLayer(doc_, layerIndex_);
}

void DeleteLayerCommand::undo() {
    // Best-effort recreate (won't restore content references or original position)
    int newIdx = mgr_->createLayer(doc_, deletedName_);
    if (newIdx >= 0) {
        mgr_->setVisible(doc_, newIdx, deletedVisible_);
    }
}

std::string DeleteLayerCommand::description() const {
    return "Delete Layer '" + deletedName_ + "'";
}

} // namespace pdfe::layers
