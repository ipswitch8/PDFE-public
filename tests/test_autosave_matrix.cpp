// -*- coding: utf-8 -*-
//
// Autosave-coverage pipeline — Phase 3 COMPREHENSIVE MATRIX.
//
// WHAT THIS IS
// ------------
// A PARAMETRIZED matrix that locks the autosave capture / restore / trigger
// contract across EVERY edit family × EVERY behavioural dimension. Phases 1-2
// established (via the discovery probe + per-gap regression locks) that
// autosave now fires on undo-dirty OR Document::contentVersion() change, writes
// Document::writeRecoveryCopy() (a fast, no-recompress full snapshot), is
// debounced on (undoStackSize, contentVersion), and that a pre-snapshot flush
// hook commits deferred manager trees (bookmarks) before the snapshot. This
// suite turns those point checks into a dense, mutation-checkable grid so a
// regression in ANY family/dimension reds a specific, named cell.
//
// Each instance drives its family EXACTLY as production drives it (through the
// real command classes via UndoManager, or — for the burn-in redaction path —
// the direct manager API that has no command), then exercises one dimension
// through an isolated recovery dir. Every oracle is a real state/content oracle
// that reopens the recovery copy in a FRESH MuPdfContext/Document (as a
// recovering process would) and asserts the edit is present / absent — never a
// tautology, never EXPECT_NO_THROW-only.
//
// DIMENSIONS
//   TriggerFires      after the edit, autosaveNow() returns true.
//   CaptureContains   recoveryFilePath() exists AND the family oracle finds the
//                     edit inside it.
//   RestoreContains   simulate a crash (drop everything un-cleanly), then a
//                     fresh scanForRecoverable() surfaces the session and the
//                     oracle finds the edit in the record's recovery copy.
//   DebounceSkip      a second autosaveNow() with NO further edit returns false.
//   UndoToPartial     apply TWO edits, undo one, autosave, reopen → recovery
//                     reflects the PARTIAL (post-undo) state (kept edit present,
//                     undone edit absent).
//   DirectReTrigger   (RedactionApply only — burn-in is not undoable) a SECOND
//                     direct mutation re-triggers autosave and the recovery copy
//                     carries BOTH burned-in regions. This replaces UndoToPartial
//                     for the non-undoable family so coverage is not reduced.
//   SaveClearsRecovery after clearRecovery() (a simulated clean save) a fresh
//                     scan no longer returns this session.
//
// A small set of CROSS-FAMILY MixedMultiEdit tests (AutosaveMatrixMixed) apply
// edits from 3+ families in one session and assert ALL survive one snapshot.
// SignatureSign is locked as the documented by-design EXCEPTION (its own
// incremental save bypasses autosave). A few AutosaveMatrixTre cells cover the
// heavy scanned-text reconstruction path.
//
// MUTATION SURVIVORS: breaking Document::writeRecoveryCopy, the contentVersion
// trigger, the debounce, or the pre-snapshot flush reds distinct, named cells.

#include <gtest/gtest.h>

#include "core/AutosaveManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "core/TextExtractor.h"

#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"

#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"

#include "redaction/RedactionManager.h"
#include "redaction/RedactionCommands.h"
#include "redaction/RedactionVerifier.h"
#include "redaction/RedactionData.h"

#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "image/ImageCommands.h"

#include "core/PageCommands.h"

#include "forms/FormManager.h"
#include "forms/FormCommands.h"
#include "forms/FormFieldData.h"

#include "metadata/MetadataManager.h"
#include "metadata/MetadataCommands.h"

#include "core/BookmarkManager.h"
#include "core/BookmarkNode.h"

#include "core/PageLabels.h"

#include "layers/LayerManager.h"

#include "signatures/SignatureManager.h"
#include "signatures/SignatureCommands.h"
#include "signatures/SignatureData.h"

// TRE (heavy path — a few cells only).
#include "tre/TreCommands.h"
#include "tre/TreTypes.h"
#include "tre/PdfContentWriter.h"
#include "tre/ImageReplacer.h"
#include "tre/OcrPipeline.h"
#include "tre/CoordMapper.h"
#include "tre/BgColorDetector.h"
#include "tre/FontMatcher.h"
#include "tre/ImageMasker.h"
#include "tre/BitmapCompositor.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/PdfFontEmbedder.h"

#include <QRectF>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

using pdfe::commands::UndoManager;
using pdfe::core::AutosaveManager;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::RecoveryRecord;

// A content oracle: given a recovery-copy path, open it FRESH and report
// whether the family's edit is present. Captures everything it needs BY VALUE
// so it stays valid after the live document/managers are dropped.
using Oracle = std::function<bool(const std::filesystem::path&)>;

// ─────────────────────────────────────────────────────────────────────────────
// Content oracles — each opens the path in its OWN context (a recovering
// process). Mirrors the audit/fix suites' oracles exactly.
// ─────────────────────────────────────────────────────────────────────────────
bool oracleText(const std::filesystem::path& path, const std::u32string& wanted) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc || doc->pageCount() == 0) return false;
    auto page = doc->page(0);
    if (!page) return false;
    pdfe::core::TextExtractor extractor(ctx);
    auto blocks = extractor.extract(page.get());
    pdfe::textedit::TextEditModel model(blocks);
    for (int i = 0; i < model.spanCount(); ++i) {
        if (model.span(i).text == wanted) return true;
    }
    return false;
}

bool oracleAnnCountOfType(const std::filesystem::path& path,
                          pdfe::annotations::AnnotationType wanted, int minCount) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc || doc->pageCount() == 0) return false;
    auto page = doc->page(0);
    if (!page) return false;
    pdfe::annotations::AnnotationManager mgr(ctx);
    int n = 0;
    for (const auto& a : mgr.getAnnotations(page.get())) {
        if (a.type == wanted) ++n;
    }
    return n >= minCount;
}

bool oracleMarkCountAtLeast(const std::filesystem::path& path, int minCount) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc || doc->pageCount() == 0) return false;
    auto page = doc->page(0);
    if (!page) return false;
    pdfe::redaction::RedactionManager mgr(ctx);
    return static_cast<int>(mgr.getRedactionMarks(page.get()).size()) >= minCount;
}

bool oracleRedactionApplied(const std::filesystem::path& path,
                            const std::vector<pdfe::redaction::RedactionData>& regions) {
    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::redaction::RedactionVerifier verifier(ctx);
    return verifier.verify(path, regions).success;
}

bool oracleImageCountAtLeast(const std::filesystem::path& path, int minCount) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc || doc->pageCount() == 0) return false;
    auto page = doc->page(0);
    if (!page) return false;
    pdfe::image::ImageManager mgr(ctx);
    return mgr.imageCount(page.get(), doc.get()) >= minCount;
}

bool oracleRotation(const std::filesystem::path& path, int wantedDegrees) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc || doc->pageCount() == 0) return false;
    auto page = doc->page(0);
    if (!page) return false;
    auto norm = [](int d) { d %= 360; if (d < 0) d += 360; return d; };
    return norm(page->rotation()) == norm(wantedDegrees);
}

bool oraclePageCount(const std::filesystem::path& path, int expected) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc) return false;
    return doc->pageCount() == expected;
}

bool oracleFormValue(const std::filesystem::path& path, int widgetIndex,
                     const std::string& wanted) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc || doc->pageCount() == 0) return false;
    auto page = doc->page(0);
    if (!page) return false;
    pdfe::forms::FormManager mgr(ctx);
    return mgr.getFormField(page.get(), widgetIndex).value == wanted;
}

bool oracleMetaTitle(const std::filesystem::path& path, const std::string& wanted) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc) return false;
    pdfe::metadata::MetadataManager mgr(ctx);
    return mgr.readMetadata(doc.get()).title == wanted;
}

bool oracleBookmarkCountAtLeast(const std::filesystem::path& path, int minCount) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc) return false;
    pdfe::core::BookmarkManager mgr(ctx);
    mgr.load(doc.get());
    return mgr.totalBookmarkCount() >= minCount;
}

bool oraclePageLabelStyle(const std::filesystem::path& path,
                          pdfe::core::PageLabelStyle style) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc) return false;
    auto rb = pdfe::core::PageLabelManager::read(doc.get());
    return !rb.empty() && rb[0].style == style;
}

bool oracleLayerNamed(const std::filesystem::path& path, const std::string& name) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc) return false;
    pdfe::layers::LayerManager mgr(ctx);
    for (const auto& l : mgr.listLayers(doc.get())) {
        if (l.name == name) return true;
    }
    return false;
}

bool oracleSignatureCountAtLeast(const std::filesystem::path& path, int minCount) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc) return false;
    pdfe::signatures::SignatureManager mgr(ctx);
    return mgr.countSignatures(doc.get()) >= minCount;
}

// ─────────────────────────────────────────────────────────────────────────────
// Matrix parameter space.
// ─────────────────────────────────────────────────────────────────────────────
enum class Family {
    TextEdit, AnnHighlight, AnnSquare, AnnInk, RedactionMark, RedactionApply,
    ImageInsert, PageRotate, PageDelete, FormFill, Metadata, Bookmarks,
    PageLabels, LayerVisibility, LayerCreate, SigFieldCreate
};

enum class Dim {
    TriggerFires, CaptureContains, RestoreContains, DebounceSkip,
    UndoToPartial, DirectReTrigger, SaveClearsRecovery
};

struct Param {
    Family family;
    Dim dim;
    std::string seed;
};

const char* familyName(Family f) {
    switch (f) {
        case Family::TextEdit:        return "TextEdit";
        case Family::AnnHighlight:    return "AnnotationHighlight";
        case Family::AnnSquare:       return "AnnotationSquare";
        case Family::AnnInk:          return "AnnotationInk";
        case Family::RedactionMark:   return "RedactionMark";
        case Family::RedactionApply:  return "RedactionApply";
        case Family::ImageInsert:     return "ImageInsert";
        case Family::PageRotate:      return "PageRotate";
        case Family::PageDelete:      return "PageDelete";
        case Family::FormFill:        return "FormFill";
        case Family::Metadata:        return "Metadata";
        case Family::Bookmarks:       return "Bookmarks";
        case Family::PageLabels:      return "PageLabels";
        case Family::LayerVisibility: return "LayerVisibility";
        case Family::LayerCreate:     return "LayerCreate";
        case Family::SigFieldCreate:  return "SignatureFieldCreate";
    }
    return "Unknown";
}

const char* dimName(Dim d) {
    switch (d) {
        case Dim::TriggerFires:       return "TriggerFires";
        case Dim::CaptureContains:    return "CaptureContains";
        case Dim::RestoreContains:    return "RestoreContains";
        case Dim::DebounceSkip:       return "DebounceSkip";
        case Dim::UndoToPartial:      return "UndoToPartial";
        case Dim::DirectReTrigger:    return "DirectReTrigger";
        case Dim::SaveClearsRecovery: return "SaveClearsRecovery";
    }
    return "Unknown";
}

// Per-family manager bag + scratch state. Managers live for the whole test body
// so a command left on the undo stack (until undo->clear()) never outlives the
// manager it references.
struct FamilyState {
    explicit FamilyState(std::shared_ptr<MuPdfContext> ctx)
        : annot(ctx), redact(ctx), image(ctx), form(ctx), meta(ctx),
          book(ctx), layer(ctx), sig(ctx) {}

    pdfe::annotations::AnnotationManager annot;
    pdfe::redaction::RedactionManager redact;
    pdfe::image::ImageManager image;
    pdfe::forms::FormManager form;
    pdfe::metadata::MetadataManager meta;
    pdfe::core::BookmarkManager book;
    pdfe::layers::LayerManager layer;
    pdfe::signatures::SignatureManager sig;

    // Scratch, seeded on edit #1 and reused on edit #2.
    int baselineCount = 0;   // annotations / marks / images / bookmarks / sigs
    int formWidgetIdx = -1;
    int formWidgetIdx2 = -1;  // a SECOND editable field (UndoToPartial needs a
                              // non-merging neighbour: two edits to the same
                              // field would merge into one undo step)
    int baseRotation = 0;
    int basePageCount = 0;
    bool bookLoaded = false;
    std::vector<pdfe::redaction::RedactionData> redRegions;
};

// ─────────────────────────────────────────────────────────────────────────────
// The parametrized fixture.
// ─────────────────────────────────────────────────────────────────────────────
class AutosaveMatrixTest : public ::testing::TestWithParam<Param> {
protected:
    void stage(const std::string& seed) {
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / seed;
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << seed << " not available";  // fixture-missing guard
        }
        const auto tmp = std::filesystem::temp_directory_path();
        const std::string tag = std::string(familyName(GetParam().family)) + "_" +
                                dimName(GetParam().dim) + "_" + seed;
        recoveryDir_ = tmp / ("pdfe_autosave_matrix_rec_" + tag);
        tempPdf_ = tmp / ("pdfe_autosave_matrix_src_" + tag);
        std::error_code ec;
        std::filesystem::remove_all(recoveryDir_, ec);
        std::filesystem::create_directories(recoveryDir_, ec);
        std::filesystem::remove(tempPdf_, ec);
        std::filesystem::copy_file(
            fixture, tempPdf_,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "stage copy failed: " << ec.message();

        ctx_ = std::make_shared<MuPdfContext>();
        doc_ = Document::open(ctx_, tempPdf_);
        ASSERT_NE(doc_, nullptr) << "Document::open failed on temp copy of " << seed;
        undo_ = std::make_unique<UndoManager>();
        // Manager constructed at OPEN (production ordering) so its
        // content-version baseline is the pristine document — required for the
        // direct (non-undo) trigger path.
        mgr_ = std::make_unique<AutosaveManager>(doc_.get(), undo_.get(),
                                                 tempPdf_, recoveryDir_);
        fam_ = std::make_unique<FamilyState>(ctx_);
        // A STABLE page-0 wrapper for the whole test. Document::page() mints a
        // fresh unique_ptr<Page> each call; commands retain a raw page pointer
        // and dereference it in undo(). In production the Page is owned by the
        // UI and outlives the commands — hold one here so a two-edit undo cannot
        // dereference a freed wrapper.
        page0_ = doc_->page(0);
    }

    void TearDown() override {
        // Ordering matters: destroy the undo stack (and thus any command still
        // on it) BEFORE the managers/document it references. clear() in the body
        // handles the happy path; this guards early-return on assertion failure.
        mgr_.reset();
        if (undo_) undo_->clear();
        undo_.reset();
        fam_.reset();
        page0_.reset();
        doc_.reset();
        ctx_.reset();
        std::error_code ec;
        std::filesystem::remove_all(recoveryDir_, ec);
        std::filesystem::remove(tempPdf_, ec);
    }

    // Read the model span at `idx` from page 0 of the live document.
    bool readSpan(int idx, pdfe::textedit::TextSpan& out, std::u32string& text) {
        auto page = doc_->page(0);
        if (!page) return false;
        pdfe::core::TextExtractor ex(ctx_);
        auto blocks = ex.extract(page.get());
        pdfe::textedit::TextEditModel m(blocks);
        if (idx >= m.spanCount()) return false;
        out = m.span(idx);
        text = out.text;
        return true;
    }

    // Apply edit number `editNo` (1 or 2) of the family, returning a by-value
    // oracle that verifies THAT edit is present in a recovery copy. editNo==1
    // seeds baselines into fam_.
    Oracle applyEdit(Family fam, int editNo);

    std::filesystem::path recoveryDir_;
    std::filesystem::path tempPdf_;
    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<UndoManager> undo_;
    std::unique_ptr<AutosaveManager> mgr_;
    std::unique_ptr<FamilyState> fam_;
    std::unique_ptr<pdfe::core::Page> page0_;
};

Oracle AutosaveMatrixTest::applyEdit(Family fam, int editNo) {
    auto failOracle = [](const std::filesystem::path&) { return false; };

    switch (fam) {
        case Family::TextEdit: {
            const int spanIdx = (editNo == 1) ? 0 : 1;
            pdfe::textedit::TextSpan span;
            std::u32string orig;
            if (!readSpan(spanIdx, span, orig)) {
                ADD_FAILURE() << "TextEdit: span " << spanIdx << " not present in seed";
                return failOracle;
            }
            const std::u32string edited =
                orig + (editNo == 1 ? U".MX1" : U".MX2");
            undo_->execute(std::make_unique<pdfe::textedit::EditTextCommand>(
                doc_.get(), ctx_, 0, spanIdx, span, orig, edited));
            return [edited](const std::filesystem::path& rp) {
                return oracleText(rp, edited);
            };
        }
        case Family::AnnHighlight:
        case Family::AnnSquare:
        case Family::AnnInk: {
            const pdfe::annotations::AnnotationType type =
                fam == Family::AnnHighlight ? pdfe::annotations::AnnotationType::Highlight
                : fam == Family::AnnSquare  ? pdfe::annotations::AnnotationType::Square
                                            : pdfe::annotations::AnnotationType::Ink;
            auto& page = page0_;
            if (editNo == 1) {
                int n = 0;
                for (const auto& a : fam_->annot.getAnnotations(page.get())) {
                    if (a.type == type) ++n;
                }
                fam_->baselineCount = n;
            }
            const QRectF rect = (editNo == 1) ? QRectF(100, 100, 120, 60)
                                              : QRectF(260, 100, 120, 60);
            undo_->execute(std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
                &fam_->annot, page.get(), type, rect));
            const int wantMin = fam_->baselineCount + editNo;
            return [type, wantMin](const std::filesystem::path& rp) {
                return oracleAnnCountOfType(rp, type, wantMin);
            };
        }
        case Family::RedactionMark: {
            auto& page = page0_;
            if (editNo == 1) {
                fam_->baselineCount =
                    static_cast<int>(fam_->redact.getRedactionMarks(page.get()).size());
            }
            const QRectF rect = (editNo == 1) ? QRectF(100, 100, 100, 50)
                                              : QRectF(250, 100, 100, 50);
            undo_->execute(std::make_unique<pdfe::redaction::CreateRedactionCommand>(
                &fam_->redact, page.get(), rect));
            const int wantMin = fam_->baselineCount + editNo;
            return [wantMin](const std::filesystem::path& rp) {
                return oracleMarkCountAtLeast(rp, wantMin);
            };
        }
        case Family::RedactionApply: {
            // DIRECT burn-in — no undo command retains the page, so a fresh
            // page handle is safe (and REQUIRED for editNo==2): applyAllRedactions
            // does a full-rewrite that invalidates the prior page handle, so the
            // stable page0_ would be stale after the first burn-in and the second
            // createRedactionMark/apply would no-op (no version bump → no
            // re-trigger). Re-resolve page 0 from the live document each call.
            auto page = doc_->page(0);
            const QRectF rect = (editNo == 1) ? QRectF(100, 660, 400, 30)
                                              : QRectF(100, 600, 400, 30);
            auto created = fam_->redact.createRedactionMark(page.get(), rect);
            fam_->redRegions.push_back(created);
            auto res = fam_->redact.applyAllRedactions(doc_.get());
            if (!res.success) {
                ADD_FAILURE() << "RedactionApply: applyAllRedactions failed: "
                              << res.errorMessage;
                return failOracle;
            }
            auto regions = fam_->redRegions;  // by value: all applied so far
            return [regions](const std::filesystem::path& rp) {
                return oracleRedactionApplied(rp, regions);
            };
        }
        case Family::ImageInsert: {
            const std::filesystem::path png =
                std::filesystem::path(TEST_FIXTURES_DIR) / "test_image_rgb.png";
            if (!std::filesystem::exists(png)) {
                ADD_FAILURE() << "ImageInsert: test_image_rgb.png not available";
                return failOracle;
            }
            auto& page = page0_;
            if (editNo == 1) {
                fam_->baselineCount = fam_->image.imageCount(page.get(), doc_.get());
            }
            pdfe::image::InsertImageOptions opts;
            opts.filePath = png.string();
            opts.targetRect = (editNo == 1) ? QRectF(60, 60, 120, 90)
                                            : QRectF(220, 60, 120, 90);
            undo_->execute(std::make_unique<pdfe::image::InsertImageCommand>(
                &fam_->image, page.get(), doc_.get(), opts));
            const int wantMin = fam_->baselineCount + editNo;
            return [wantMin](const std::filesystem::path& rp) {
                return oracleImageCountAtLeast(rp, wantMin);
            };
        }
        case Family::PageRotate: {
            if (editNo == 1) {
                auto& page = page0_;
                fam_->baseRotation = page->rotation();
            }
            undo_->execute(std::make_unique<pdfe::commands::RotatePageCommand>(
                doc_.get(), 0, 90));
            const int expected = fam_->baseRotation + 90 * editNo;
            return [expected](const std::filesystem::path& rp) {
                return oracleRotation(rp, expected);
            };
        }
        case Family::PageDelete: {
            if (editNo == 1) {
                fam_->basePageCount = doc_->pageCount();
            }
            const int idx = fam_->basePageCount - editNo;  // delete current last
            undo_->execute(std::make_unique<pdfe::commands::DeletePageCommand>(
                doc_.get(), idx));
            const int expected = fam_->basePageCount - editNo;
            return [expected](const std::filesystem::path& rp) {
                return oraclePageCount(rp, expected);
            };
        }
        case Family::FormFill: {
            auto& page = page0_;
            if (editNo == 1) {
                // Collect up to two DISTINCT editable text fields. Edit #2 must
                // target a DIFFERENT field: two SetFieldValueCommands on the
                // same widget MERGE into one undo step (production coalesces
                // consecutive typing), which would defeat UndoToPartial.
                for (const auto& f : fam_->form.getFormFields(page.get())) {
                    if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
                        if (fam_->formWidgetIdx < 0) {
                            fam_->formWidgetIdx = f.widgetIndex;
                        } else if (f.widgetIndex != fam_->formWidgetIdx) {
                            fam_->formWidgetIdx2 = f.widgetIndex;
                            break;
                        }
                    }
                }
                if (fam_->formWidgetIdx < 0) {
                    ADD_FAILURE() << "FormFill: no editable text field in seed";
                    return failOracle;
                }
                if (fam_->formWidgetIdx2 < 0) {
                    // Fall back to the first field for the second slot; only the
                    // single-edit dimensions use editNo==1, so this still leaves
                    // every dimension genuinely exercised for such seeds.
                    fam_->formWidgetIdx2 = fam_->formWidgetIdx;
                }
            }
            const int idx = (editNo == 1) ? fam_->formWidgetIdx : fam_->formWidgetIdx2;
            const std::string val = (editNo == 1) ? "MATRIX_FF_1" : "MATRIX_FF_2";
            undo_->execute(std::make_unique<pdfe::forms::SetFieldValueCommand>(
                &fam_->form, page.get(), idx, val));
            return [idx, val](const std::filesystem::path& rp) {
                return oracleFormValue(rp, idx, val);
            };
        }
        case Family::Metadata: {
            auto meta = fam_->meta.readMetadata(doc_.get());
            const std::string title = (editNo == 1) ? "MATRIX_MD_1" : "MATRIX_MD_2";
            meta.title = title;
            undo_->execute(std::make_unique<pdfe::metadata::EditMetadataCommand>(
                &fam_->meta, doc_.get(), meta));
            return [title](const std::filesystem::path& rp) {
                return oracleMetaTitle(rp, title);
            };
        }
        case Family::Bookmarks: {
            if (!fam_->bookLoaded) {
                fam_->book.load(doc_.get());
                fam_->baselineCount = fam_->book.totalBookmarkCount();
                fam_->bookLoaded = true;
                // Wire the pre-snapshot flush so the in-panel bookmark tree is
                // committed into the live document just before the snapshot.
                mgr_->setPreSnapshotFlush([this]() { fam_->book.commit(doc_.get()); });
            }
            pdfe::core::BookmarkDest dest;
            dest.type = pdfe::core::BookmarkDestType::Page;
            dest.pageIndex = 0;
            const std::string title = (editNo == 1) ? "MatrixBM1" : "MatrixBM2";
            undo_->execute(std::make_unique<pdfe::core::CreateBookmarkCommand>(
                &fam_->book, pdfe::core::BookmarkPath{}, title, dest));
            const int wantMin = fam_->baselineCount + editNo;
            return [wantMin](const std::filesystem::path& rp) {
                return oracleBookmarkCountAtLeast(rp, wantMin);
            };
        }
        case Family::PageLabels: {
            const pdfe::core::PageLabelStyle style =
                (editNo == 1) ? pdfe::core::PageLabelStyle::RomanUpper
                              : pdfe::core::PageLabelStyle::Decimal;
            std::vector<pdfe::core::PageLabelRange> ranges = {
                {0, style, "", 1},
            };
            undo_->execute(std::make_unique<pdfe::core::EditPageLabelsCommand>(
                doc_.get(), ranges));
            return [style](const std::filesystem::path& rp) {
                return oraclePageLabelStyle(rp, style);
            };
        }
        case Family::LayerCreate: {
            const std::string name =
                (editNo == 1) ? "MatrixLayerA" : "MatrixLayerB";
            undo_->execute(std::make_unique<pdfe::layers::CreateLayerCommand>(
                &fam_->layer, doc_.get(), name));
            return [name](const std::filesystem::path& rp) {
                return oracleLayerNamed(rp, name);
            };
        }
        case Family::LayerVisibility: {
            // One logical edit = create a uniquely-named layer, then toggle its
            // visibility via SetLayerVisibilityCommand (the production layer-edit
            // path that dirties the undo stack). Two undo commands per edit.
            const std::string name =
                (editNo == 1) ? "MatrixVisA" : "MatrixVisB";
            undo_->execute(std::make_unique<pdfe::layers::CreateLayerCommand>(
                &fam_->layer, doc_.get(), name));
            int layerIdx = -1;
            for (const auto& l : fam_->layer.listLayers(doc_.get())) {
                if (l.name == name) { layerIdx = l.index; break; }
            }
            if (layerIdx < 0) {
                ADD_FAILURE() << "LayerVisibility: created layer not listed";
                return failOracle;
            }
            undo_->execute(std::make_unique<pdfe::layers::SetLayerVisibilityCommand>(
                &fam_->layer, doc_.get(), layerIdx, /*visible=*/false));
            return [name](const std::filesystem::path& rp) {
                return oracleLayerNamed(rp, name);
            };
        }
        case Family::SigFieldCreate: {
            auto& page = page0_;
            if (editNo == 1) {
                fam_->baselineCount = fam_->sig.countSignatures(doc_.get());
            }
            const std::string name = (editNo == 1) ? "MatrixSigA" : "MatrixSigB";
            const QRectF rect = (editNo == 1) ? QRectF(100, 500, 200, 50)
                                              : QRectF(100, 430, 200, 50);
            undo_->execute(std::make_unique<pdfe::signatures::CreateSignatureFieldCommand>(
                &fam_->sig, page.get(), name, rect));
            const int wantMin = fam_->baselineCount + editNo;
            return [wantMin](const std::filesystem::path& rp) {
                return oracleSignatureCountAtLeast(rp, wantMin);
            };
        }
    }
    ADD_FAILURE() << "unhandled family";
    return failOracle;
}

TEST_P(AutosaveMatrixTest, Cell) {
    const Param prm = GetParam();
    stage(prm.seed);

    switch (prm.dim) {
        case Dim::TriggerFires: {
            (void)applyEdit(prm.family, 1);
            ASSERT_TRUE(mgr_->autosaveNow())
                << familyName(prm.family) << " edit must trigger an autosave";
            undo_->clear();
        } break;

        case Dim::CaptureContains: {
            Oracle o1 = applyEdit(prm.family, 1);
            ASSERT_TRUE(mgr_->autosaveNow());
            ASSERT_TRUE(std::filesystem::exists(mgr_->recoveryFilePath()))
                << "recovery copy must be written";
            EXPECT_TRUE(o1(mgr_->recoveryFilePath()))
                << "recovery copy must carry the " << familyName(prm.family) << " edit";
            undo_->clear();
        } break;

        case Dim::RestoreContains: {
            Oracle o1 = applyEdit(prm.family, 1);
            ASSERT_TRUE(mgr_->autosaveNow());
            const std::string sid = mgr_->sessionId();
            const std::filesystem::path recDir = recoveryDir_;
            // Release commands while managers/doc are alive, then SIMULATE CRASH:
            // drop everything un-cleanly (no clearRecovery / no clean save).
            undo_->clear();
            mgr_.reset();
            undo_.reset();
            fam_.reset();
            page0_.reset();
            doc_.reset();
            // Fresh scan (as a recovering process at launch) then reopen.
            auto found = AutosaveManager::scanForRecoverable(recDir);
            ASSERT_FALSE(found.empty()) << "scan must surface the crashed session";
            bool matched = false;
            for (const auto& rec : found) {
                if (rec.sessionId == sid) {
                    matched = true;
                    EXPECT_TRUE(o1(rec.recoveryFilePath))
                        << "restored recovery copy must carry the edit";
                    break;
                }
            }
            EXPECT_TRUE(matched) << "no recovery record for this session";
        } break;

        case Dim::DebounceSkip: {
            (void)applyEdit(prm.family, 1);
            ASSERT_TRUE(mgr_->autosaveNow()) << "first autosave must write";
            EXPECT_FALSE(mgr_->autosaveNow())
                << "no further edit → debounce must skip the redundant rewrite";
            undo_->clear();
        } break;

        case Dim::UndoToPartial: {
            Oracle o1 = applyEdit(prm.family, 1);
            const int sizeBeforeEdit2 = undo_->undoStackSize();
            Oracle o2 = applyEdit(prm.family, 2);
            // Pop exactly edit #2 (which may be >1 command, e.g. LayerVisibility).
            while (undo_->undoStackSize() > sizeBeforeEdit2 && undo_->canUndo()) {
                undo_->undo();
            }
            ASSERT_TRUE(mgr_->autosaveNow())
                << "the partial (post-undo) state must still autosave";
            const auto rp = mgr_->recoveryFilePath();
            ASSERT_TRUE(std::filesystem::exists(rp));
            EXPECT_TRUE(o1(rp)) << "the KEPT edit must be in the recovery copy";
            EXPECT_FALSE(o2(rp)) << "the UNDONE edit must NOT be in the recovery copy";
            undo_->clear();
        } break;

        case Dim::DirectReTrigger: {
            (void)applyEdit(prm.family, 1);
            ASSERT_TRUE(mgr_->autosaveNow()) << "first direct mutation must autosave";
            Oracle o2 = applyEdit(prm.family, 2);
            ASSERT_TRUE(mgr_->autosaveNow())
                << "a SECOND direct mutation must re-trigger autosave (content-version)";
            const auto rp = mgr_->recoveryFilePath();
            ASSERT_TRUE(std::filesystem::exists(rp));
            EXPECT_TRUE(o2(rp)) << "recovery copy must carry BOTH burned-in regions";
            undo_->clear();
        } break;

        case Dim::SaveClearsRecovery: {
            (void)applyEdit(prm.family, 1);
            ASSERT_TRUE(mgr_->autosaveNow());
            const std::string sid = mgr_->sessionId();
            ASSERT_TRUE(mgr_->hasRecovery());
            {
                auto found = AutosaveManager::scanForRecoverable(recoveryDir_);
                bool present = false;
                for (const auto& r : found) if (r.sessionId == sid) present = true;
                ASSERT_TRUE(present) << "session must be recoverable before clear";
            }
            mgr_->clearRecovery();  // simulate a clean save
            EXPECT_FALSE(mgr_->hasRecovery())
                << "clearRecovery() must remove this session's sidecar";
            auto found = AutosaveManager::scanForRecoverable(recoveryDir_);
            for (const auto& r : found) {
                EXPECT_NE(r.sessionId, sid)
                    << "a cleanly-saved session must not be recoverable";
            }
            undo_->clear();
        } break;
    }
}

// Sanitize a fixture name into a gtest-safe token (drops a trailing ".pdf").
std::string sanitize(const std::string& in) {
    std::string s = in;
    if (s.size() >= 4 && s.compare(s.size() - 4, 4, ".pdf") == 0) {
        s.erase(s.size() - 4);
    }
    std::string out;
    for (char c : s) {
        out += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    return out;
}

std::vector<Param> makeParams() {
    // Standard undoable dimensions.
    const std::vector<Dim> undoableDims = {
        Dim::TriggerFires, Dim::CaptureContains, Dim::RestoreContains,
        Dim::DebounceSkip, Dim::UndoToPartial, Dim::SaveClearsRecovery};
    // Non-undoable (redaction burn-in): swap UndoToPartial → DirectReTrigger so
    // coverage is not reduced (SKIP=FAIL — no padding).
    const std::vector<Dim> directDims = {
        Dim::TriggerFires, Dim::CaptureContains, Dim::RestoreContains,
        Dim::DebounceSkip, Dim::DirectReTrigger, Dim::SaveClearsRecovery};

    struct FamCfg {
        Family family;
        std::vector<std::string> seeds;
        const std::vector<Dim>* dims;
    };
    const std::vector<FamCfg> cfgs = {
        {Family::TextEdit,     {"test_3page.pdf", "test_redaction.pdf", "test_with_bookmarks.pdf"}, &undoableDims},
        {Family::AnnHighlight, {"test_3page.pdf", "test_single.pdf"}, &undoableDims},
        {Family::AnnSquare,    {"test_3page.pdf", "test_single.pdf"}, &undoableDims},
        {Family::AnnInk,       {"test_3page.pdf", "test_single.pdf"}, &undoableDims},
        {Family::RedactionMark,{"test_redaction.pdf", "test_3page.pdf"}, &undoableDims},
        {Family::RedactionApply,{"test_redaction.pdf"}, &directDims},
        {Family::ImageInsert,  {"test_pdf_with_images.pdf", "test_3page.pdf"}, &undoableDims},
        {Family::PageRotate,   {"test_3page.pdf", "test_10page.pdf"}, &undoableDims},
        {Family::PageDelete,   {"test_3page.pdf", "test_10page.pdf"}, &undoableDims},
        {Family::FormFill,     {"test_form.pdf", "test_form_with_dv.pdf"}, &undoableDims},
        {Family::Metadata,     {"test_single.pdf", "test_3page.pdf"}, &undoableDims},
        {Family::Bookmarks,    {"test_with_bookmarks.pdf", "test_3page.pdf"}, &undoableDims},
        {Family::PageLabels,   {"test_3page.pdf", "test_10page.pdf"}, &undoableDims},
        {Family::LayerVisibility, {"test_3page.pdf"}, &undoableDims},
        {Family::LayerCreate,  {"test_3page.pdf", "test_single.pdf"}, &undoableDims},
        {Family::SigFieldCreate,{"test_single.pdf", "test_3page.pdf"}, &undoableDims},
    };

    std::vector<Param> out;
    for (const auto& cfg : cfgs) {
        for (const std::string& seed : cfg.seeds) {
            for (Dim d : *cfg.dims) {
                out.push_back(Param{cfg.family, d, seed});
            }
        }
    }
    return out;
}

std::string paramName(const ::testing::TestParamInfo<Param>& info) {
    return std::string(familyName(info.param.family)) + "_" +
           dimName(info.param.dim) + "_" + sanitize(info.param.seed);
}

INSTANTIATE_TEST_SUITE_P(AutosaveMatrix, AutosaveMatrixTest,
                         ::testing::ValuesIn(makeParams()), paramName);

// ─────────────────────────────────────────────────────────────────────────────
// CROSS-FAMILY: 3+ families edited in one session, one snapshot captures all.
// ─────────────────────────────────────────────────────────────────────────────
class AutosaveMatrixMixed : public ::testing::Test {
protected:
    void stage(const std::string& seed) {
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / seed;
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << seed << " not available";
        }
        const auto tmp = std::filesystem::temp_directory_path();
        recoveryDir_ = tmp / ("pdfe_autosave_mixed_rec_" + sanitize(seed));
        tempPdf_ = tmp / ("pdfe_autosave_mixed_src_" + sanitize(seed) + ".pdf");
        std::error_code ec;
        std::filesystem::remove_all(recoveryDir_, ec);
        std::filesystem::create_directories(recoveryDir_, ec);
        std::filesystem::remove(tempPdf_, ec);
        std::filesystem::copy_file(
            fixture, tempPdf_,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "stage copy failed: " << ec.message();
        ctx_ = std::make_shared<MuPdfContext>();
        doc_ = Document::open(ctx_, tempPdf_);
        ASSERT_NE(doc_, nullptr);
        undo_ = std::make_unique<UndoManager>();
        mgr_ = std::make_unique<AutosaveManager>(doc_.get(), undo_.get(),
                                                 tempPdf_, recoveryDir_);
    }
    void TearDown() override {
        mgr_.reset();
        if (undo_) undo_->clear();
        undo_.reset();
        doc_.reset();
        ctx_.reset();
        std::error_code ec;
        std::filesystem::remove_all(recoveryDir_, ec);
        std::filesystem::remove(tempPdf_, ec);
    }
    std::filesystem::path recoveryDir_, tempPdf_;
    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<UndoManager> undo_;
    std::unique_ptr<AutosaveManager> mgr_;
};

TEST_F(AutosaveMatrixMixed, TextAnnotationMetadata) {
    stage("test_3page.pdf");
    pdfe::annotations::AnnotationManager amgr(ctx_);
    pdfe::metadata::MetadataManager mmgr(ctx_);

    // TextEdit span 0.
    pdfe::textedit::TextSpan span;
    std::u32string orig;
    {
        auto page = doc_->page(0);
        pdfe::core::TextExtractor ex(ctx_);
        auto blocks = ex.extract(page.get());
        pdfe::textedit::TextEditModel m(blocks);
        ASSERT_GT(m.spanCount(), 0);
        span = m.span(0);
        orig = span.text;
    }
    const std::u32string edited = orig + U".MIX";
    undo_->execute(std::make_unique<pdfe::textedit::EditTextCommand>(
        doc_.get(), ctx_, 0, 0, span, orig, edited));
    // Square annotation.
    undo_->execute(std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
        &amgr, doc_->page(0).get(), pdfe::annotations::AnnotationType::Square,
        QRectF(100, 100, 200, 80)));
    // Metadata title.
    auto meta = mmgr.readMetadata(doc_.get());
    meta.title = "MIX_TITLE";
    undo_->execute(std::make_unique<pdfe::metadata::EditMetadataCommand>(
        &mmgr, doc_.get(), meta));

    ASSERT_TRUE(mgr_->autosaveNow());
    const auto rp = mgr_->recoveryFilePath();
    EXPECT_TRUE(oracleText(rp, edited));
    EXPECT_TRUE(oracleAnnCountOfType(rp, pdfe::annotations::AnnotationType::Square, 1));
    EXPECT_TRUE(oracleMetaTitle(rp, "MIX_TITLE"));
    undo_->clear();
}

TEST_F(AutosaveMatrixMixed, HighlightRotateLayer) {
    stage("test_3page.pdf");
    pdfe::annotations::AnnotationManager amgr(ctx_);
    pdfe::layers::LayerManager lmgr(ctx_);

    int baseRot = doc_->page(0)->rotation();
    undo_->execute(std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
        &amgr, doc_->page(0).get(), pdfe::annotations::AnnotationType::Highlight,
        QRectF(80, 80, 160, 40)));
    undo_->execute(std::make_unique<pdfe::commands::RotatePageCommand>(
        doc_.get(), 0, 90));
    undo_->execute(std::make_unique<pdfe::layers::CreateLayerCommand>(
        &lmgr, doc_.get(), "MixLayer"));

    ASSERT_TRUE(mgr_->autosaveNow());
    const auto rp = mgr_->recoveryFilePath();
    EXPECT_TRUE(oracleAnnCountOfType(rp, pdfe::annotations::AnnotationType::Highlight, 1));
    EXPECT_TRUE(oracleRotation(rp, baseRot + 90));
    EXPECT_TRUE(oracleLayerNamed(rp, "MixLayer"));
    undo_->clear();
}

TEST_F(AutosaveMatrixMixed, FormMetadataAnnotation) {
    stage("test_form.pdf");
    pdfe::forms::FormManager fmgr(ctx_);
    pdfe::metadata::MetadataManager mmgr(ctx_);
    pdfe::annotations::AnnotationManager amgr(ctx_);

    int widgetIdx = -1;
    for (const auto& f : fmgr.getFormFields(doc_->page(0).get())) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            widgetIdx = f.widgetIndex;
            break;
        }
    }
    ASSERT_GE(widgetIdx, 0) << "seed must have an editable text field";
    undo_->execute(std::make_unique<pdfe::forms::SetFieldValueCommand>(
        &fmgr, doc_->page(0).get(), widgetIdx, "MIX_FORM"));
    auto meta = mmgr.readMetadata(doc_.get());
    meta.title = "MIX_FORM_TITLE";
    undo_->execute(std::make_unique<pdfe::metadata::EditMetadataCommand>(
        &mmgr, doc_.get(), meta));
    undo_->execute(std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
        &amgr, doc_->page(0).get(), pdfe::annotations::AnnotationType::Ink,
        QRectF(50, 50, 100, 60)));

    ASSERT_TRUE(mgr_->autosaveNow());
    const auto rp = mgr_->recoveryFilePath();
    EXPECT_TRUE(oracleFormValue(rp, widgetIdx, "MIX_FORM"));
    EXPECT_TRUE(oracleMetaTitle(rp, "MIX_FORM_TITLE"));
    EXPECT_TRUE(oracleAnnCountOfType(rp, pdfe::annotations::AnnotationType::Ink, 1));
    undo_->clear();
}

TEST_F(AutosaveMatrixMixed, RedactionMarkSignatureMetadata) {
    stage("test_3page.pdf");
    pdfe::redaction::RedactionManager rmgr(ctx_);
    pdfe::signatures::SignatureManager smgr(ctx_);
    pdfe::metadata::MetadataManager mmgr(ctx_);

    const int sigBase = smgr.countSignatures(doc_.get());
    undo_->execute(std::make_unique<pdfe::redaction::CreateRedactionCommand>(
        &rmgr, doc_->page(0).get(), QRectF(100, 100, 100, 50)));
    undo_->execute(std::make_unique<pdfe::signatures::CreateSignatureFieldCommand>(
        &smgr, doc_->page(0).get(), "MixSig", QRectF(100, 500, 200, 50)));
    auto meta = mmgr.readMetadata(doc_.get());
    meta.title = "MIX_RSM";
    undo_->execute(std::make_unique<pdfe::metadata::EditMetadataCommand>(
        &mmgr, doc_.get(), meta));

    ASSERT_TRUE(mgr_->autosaveNow());
    const auto rp = mgr_->recoveryFilePath();
    EXPECT_TRUE(oracleMarkCountAtLeast(rp, 1));
    EXPECT_TRUE(oracleSignatureCountAtLeast(rp, sigBase + 1));
    EXPECT_TRUE(oracleMetaTitle(rp, "MIX_RSM"));
    undo_->clear();
}

TEST_F(AutosaveMatrixMixed, ImageMetadataAnnotation) {
    stage("test_pdf_with_images.pdf");
    const std::filesystem::path png =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_image_rgb.png";
    if (!std::filesystem::exists(png)) GTEST_SKIP() << "test_image_rgb.png missing";
    pdfe::image::ImageManager imgr(ctx_);
    pdfe::metadata::MetadataManager mmgr(ctx_);
    pdfe::annotations::AnnotationManager amgr(ctx_);

    const int imgBase = imgr.imageCount(doc_->page(0).get(), doc_.get());
    pdfe::image::InsertImageOptions opts;
    opts.filePath = png.string();
    opts.targetRect = QRectF(60, 60, 120, 90);
    undo_->execute(std::make_unique<pdfe::image::InsertImageCommand>(
        &imgr, doc_->page(0).get(), doc_.get(), opts));
    auto meta = mmgr.readMetadata(doc_.get());
    meta.title = "MIX_IMG";
    undo_->execute(std::make_unique<pdfe::metadata::EditMetadataCommand>(
        &mmgr, doc_.get(), meta));
    undo_->execute(std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
        &amgr, doc_->page(0).get(), pdfe::annotations::AnnotationType::Square,
        QRectF(200, 200, 80, 80)));

    ASSERT_TRUE(mgr_->autosaveNow());
    const auto rp = mgr_->recoveryFilePath();
    EXPECT_TRUE(oracleImageCountAtLeast(rp, imgBase + 1));
    EXPECT_TRUE(oracleMetaTitle(rp, "MIX_IMG"));
    EXPECT_TRUE(oracleAnnCountOfType(rp, pdfe::annotations::AnnotationType::Square, 1));
    undo_->clear();
}

TEST_F(AutosaveMatrixMixed, BookmarkTextLayer) {
    stage("test_with_bookmarks.pdf");
    pdfe::core::BookmarkManager bmgr(ctx_);
    pdfe::layers::LayerManager lmgr(ctx_);
    bmgr.load(doc_.get());
    const int bmBase = bmgr.totalBookmarkCount();

    pdfe::core::BookmarkDest dest;
    dest.type = pdfe::core::BookmarkDestType::Page;
    dest.pageIndex = 0;
    undo_->execute(std::make_unique<pdfe::core::CreateBookmarkCommand>(
        &bmgr, pdfe::core::BookmarkPath{}, "MixBookmark", dest));
    mgr_->setPreSnapshotFlush([&]() { bmgr.commit(doc_.get()); });

    pdfe::textedit::TextSpan span;
    std::u32string orig;
    {
        auto page = doc_->page(0);
        pdfe::core::TextExtractor ex(ctx_);
        auto blocks = ex.extract(page.get());
        pdfe::textedit::TextEditModel m(blocks);
        ASSERT_GT(m.spanCount(), 0);
        span = m.span(0);
        orig = span.text;
    }
    const std::u32string edited = orig + U".MIXBM";
    undo_->execute(std::make_unique<pdfe::textedit::EditTextCommand>(
        doc_.get(), ctx_, 0, 0, span, orig, edited));
    undo_->execute(std::make_unique<pdfe::layers::CreateLayerCommand>(
        &lmgr, doc_.get(), "MixBmLayer"));

    ASSERT_TRUE(mgr_->autosaveNow());
    const auto rp = mgr_->recoveryFilePath();
    EXPECT_TRUE(oracleBookmarkCountAtLeast(rp, bmBase + 1));
    EXPECT_TRUE(oracleText(rp, edited));
    EXPECT_TRUE(oracleLayerNamed(rp, "MixBmLayer"));
    undo_->clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// BY-DESIGN EXCEPTION: SignatureSign is NOT captured by autosave.
//
// SignatureManager::signField() takes a const Document*, pushes NO undo command,
// and performs its OWN incremental save (pdf_save_document) directly to the
// source file. It therefore neither dirties the undo stack NOR moves the tracked
// Document::contentVersion(), so autosaveNow() does not fire — the signature is
// persisted through the signing path itself, not the recovery snapshot. This
// test LOCKS that exception (so it is explicit, not silently absent).
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveMatrixSignatureException, SignIsNotCapturedByAutosave) {
    std::filesystem::path fixture =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "test_single.pdf missing";
    const std::filesystem::path pfx =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_signer.pfx";
    if (!std::filesystem::exists(pfx)) GTEST_SKIP() << "test_signer.pfx not available";

    const auto tmp = std::filesystem::temp_directory_path();
    const auto recDir = tmp / "pdfe_autosave_matrix_sigexc_rec";
    const auto src = tmp / "pdfe_autosave_matrix_sigexc_src.pdf";
    std::error_code ec;
    std::filesystem::remove_all(recDir, ec);
    std::filesystem::create_directories(recDir, ec);
    std::filesystem::remove(src, ec);
    std::filesystem::copy_file(fixture, src,
                               std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec);

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, src);
    ASSERT_NE(doc, nullptr);
    auto undo = std::make_unique<UndoManager>();
    // Manager at open → pristine content-version baseline.
    AutosaveManager mgr(doc.get(), undo.get(), src, recDir);

    pdfe::signatures::SignatureManager smgr(ctx);
    if (!smgr.isPkcs7BridgeAvailable()) GTEST_SKIP() << "PKCS7 bridge unavailable";

    auto page = doc->page(0);
    smgr.createSignatureField(page.get(), "SignMe", QRectF(100, 500, 200, 50));

    pdfe::signatures::SigningOptions opts;
    opts.pfxPath = pfx.string();
    opts.pfxPassword = "test123";
    opts.reason = "Autosave exception lock";
    opts.location = "Phase 3";
    try {
        smgr.signField(doc.get(), page.get(), 0, opts);
    } catch (const std::exception& e) {
        GTEST_SKIP() << "signField unavailable in this build: " << e.what();
    }

    // The signing pushed no command — the undo stack is clean.
    EXPECT_TRUE(undo->isClean())
        << "signing must not push an undo command (by design)";
    // And autosave does NOT fire: the signature was persisted by signField's own
    // incremental save, bypassing the undo/content-version-driven autosave.
    EXPECT_FALSE(mgr.autosaveNow())
        << "signing must NOT trigger an autosave (its own incremental save owns "
           "persistence — the documented by-design exception)";
    EXPECT_FALSE(std::filesystem::exists(mgr.recoveryFilePath()))
        << "no recovery copy is written for a signing operation";

    // Positive control: the signature DID persist via signField's own path.
    EXPECT_TRUE(oracleSignatureCountAtLeast(src, 1))
        << "signField's own incremental save must have written the signature";

    mgr.stop();
    undo.reset();
    doc.reset();
    ctx.reset();
    std::filesystem::remove_all(recDir, ec);
    std::filesystem::remove(src, ec);
}

// ─────────────────────────────────────────────────────────────────────────────
// HEAVY PATH: TRE scanned-text reconstruction — a FEW cells only (~6s each).
// Guarded on tessdata (fixture-missing guard). Mirrors the audit's TRE stack.
// ─────────────────────────────────────────────────────────────────────────────
class AutosaveMatrixTre : public ::testing::Test {
protected:
    // Returns true and fills rp/o with a reconstruction result, or GTEST_SKIPs
    // when tessdata / the scanned fixture are unavailable.
    struct TreResult {
        bool ran = false;
        std::filesystem::path recoveryFile;
        std::filesystem::path recoveryDir;
        std::string sessionId;
        std::vector<uint8_t> originalImage;
    };
};

namespace {
bool treImageChanged(const std::filesystem::path& rp,
                     const std::vector<uint8_t>& orig) {
    auto octx = std::make_shared<MuPdfContext>();
    auto odoc = Document::open(octx, rp);
    if (!odoc || odoc->pageCount() == 0) return false;
    auto oembed = std::make_unique<pdfe::fonts::PdfFontEmbedder>(octx.get());
    pdfe::tre::PdfContentWriter owriter(octx.get(), oembed.get());
    auto page = odoc->page(0);
    auto data = owriter.getImageXObjectData(page.get(), odoc.get());
    return !data.empty() && data != orig;
}
}  // namespace

TEST_F(AutosaveMatrixTre, TriggerCaptureRestore) {
    const std::filesystem::path tessdata =
        std::filesystem::path(TEST_FIXTURES_DIR) / "tessdata";
    if (!std::filesystem::exists(tessdata / "eng.traineddata")) {
        GTEST_SKIP() << "tessdata/eng.traineddata not available";
    }
    std::filesystem::path fixture =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_scanned.pdf";
    if (!std::filesystem::exists(fixture)) GTEST_SKIP() << "test_scanned.pdf missing";

    const auto tmp = std::filesystem::temp_directory_path();
    const auto recDir = tmp / "pdfe_autosave_matrix_tre_rec";
    const auto src = tmp / "pdfe_autosave_matrix_tre_src.pdf";
    std::error_code ec;
    std::filesystem::remove_all(recDir, ec);
    std::filesystem::create_directories(recDir, ec);
    std::filesystem::remove(src, ec);
    std::filesystem::copy_file(fixture, src,
                               std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec);

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, src);
    ASSERT_NE(doc, nullptr);
    auto undo = std::make_unique<UndoManager>();
    auto mgr = std::make_unique<AutosaveManager>(doc.get(), undo.get(), src, recDir);

    std::vector<uint8_t> originalData;
    std::string sid;
    std::filesystem::path recFile;
    {
        auto analyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(ctx.get());
        auto fontCache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
        auto selector = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
            analyzer.get(), fontCache.get());
        auto embedder = std::make_unique<pdfe::fonts::PdfFontEmbedder>(ctx.get());
        pdfe::tre::OcrConfig ocrConfig;
        ocrConfig.tessdataPath = tessdata.string();
        auto ocrPipeline = std::make_unique<pdfe::tre::OcrPipeline>(
            ctx.get(), doc.get(), ocrConfig);
        auto coordMapper = std::make_unique<pdfe::tre::CoordMapper>(ctx.get(), doc.get());
        auto bgDetector = std::make_unique<pdfe::tre::BgColorDetector>();
        auto fontMatcher = std::make_unique<pdfe::tre::FontMatcher>(
            selector.get(), analyzer.get());
        auto imageMasker = std::make_unique<pdfe::tre::ImageMasker>();
        auto bitmapCompositor = std::make_unique<pdfe::tre::BitmapCompositor>();
        auto contentWriter = std::make_unique<pdfe::tre::PdfContentWriter>(
            ctx.get(), embedder.get());
        auto imageReplacer = std::make_unique<pdfe::tre::ImageReplacer>(
            ocrPipeline.get(), coordMapper.get(), bgDetector.get(),
            fontMatcher.get(), imageMasker.get(), bitmapCompositor.get(),
            contentWriter.get());

        {
            auto page = doc->page(0);
            originalData = contentWriter->getImageXObjectData(page.get(), doc.get());
        }
        ASSERT_FALSE(originalData.empty()) << "no image XObject on page 0";

        pdfe::tre::PdfRect selection{50, 50, 500, 700};
        undo->execute(std::make_unique<pdfe::tre::ImageReplaceCommand>(
            doc.get(), ctx, imageReplacer.get(), contentWriter.get(),
            0, selection, "AUTOSAVE MATRIX TRE", pdfe::tre::RegionId{1}));

        // TriggerFires + CaptureContains.
        ASSERT_TRUE(mgr->autosaveNow()) << "TRE reconstruction must trigger autosave";
        recFile = mgr->recoveryFilePath();
        sid = mgr->sessionId();
        ASSERT_TRUE(std::filesystem::exists(recFile));
        EXPECT_TRUE(treImageChanged(recFile, originalData))
            << "recovery copy must carry the reconstructed image";

        undo->clear();  // release command while its TRE stack is still alive
    }

    // RestoreContains: simulate crash, scan, reopen.
    mgr.reset();
    undo.reset();
    doc.reset();
    ctx.reset();
    auto found = AutosaveManager::scanForRecoverable(recDir);
    ASSERT_FALSE(found.empty());
    bool matched = false;
    for (const auto& rec : found) {
        if (rec.sessionId == sid) {
            matched = true;
            EXPECT_TRUE(treImageChanged(rec.recoveryFilePath, originalData))
                << "restored TRE recovery copy must carry the reconstruction";
            break;
        }
    }
    EXPECT_TRUE(matched);

    std::filesystem::remove_all(recDir, ec);
    std::filesystem::remove(src, ec);
}

}  // namespace
