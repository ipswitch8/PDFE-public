// -*- coding: utf-8 -*-
// Phase-4 fuzz harness: embedded-files / portfolio enumeration.
//
// Exercises AttachmentManager::list + isPortfolio + portfolioView.
// Targets PDFE's /EmbeddedFiles name-tree walk and /Collection
// dict parsing -- both attacker-controlled paths that have to deal
// with cyclic name-tree refs and malformed UTF-16 filenames.
#include "fuzz_common.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "attachments/AttachmentManager.h"

#include <memory>

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data,
                                    std::size_t size)
{
    if (size == 0 || size > 4 * 1024 * 1024) return;
    pdfe::fuzz::TempPdfFile tmp(data, size);
    if (!tmp.valid()) return;

    try {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, tmp.path());
        if (!doc) return;
        pdfe::attachments::AttachmentManager mgr(ctx);
        auto entries = mgr.list(doc.get());
        (void)entries;
        bool portfolio = mgr.isPortfolio(doc.get());
        (void)portfolio;
        std::string view = mgr.portfolioView(doc.get());
        (void)view;
    } catch (...) {
    }
}
