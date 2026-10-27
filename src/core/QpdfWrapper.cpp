#include "QpdfWrapper.h"

#include <qpdf/QPDFPageDocumentHelper.hh>

namespace pdfe::core {

void QpdfWrapper::open(const std::filesystem::path& path, const std::string& password)
{
    try {
        qpdf_ = std::make_unique<QPDF>();
        if (password.empty()) {
            qpdf_->processFile(path.string().c_str());
        } else {
            qpdf_->processFile(path.string().c_str(), password.c_str());
        }
        currentPath_ = path;
    } catch (const std::exception& e) {
        qpdf_.reset();
        throw QpdfException(std::string("QPDF open failed: ") + e.what());
    }
}

void QpdfWrapper::openFromStream(std::FILE* file, const std::string& description,
                                 const std::string& password)
{
    if (!file) {
        throw QpdfException("QPDF open failed: null stream");
    }
    try {
        qpdf_ = std::make_unique<QPDF>();
        // close_file = true: QPDF owns the stream from here, including on the
        // throwing path below (processFile closes it before propagating).
        qpdf_->processFile(description.c_str(), file, /*close_file=*/true,
                           password.empty() ? nullptr : password.c_str());
        currentPath_ = std::filesystem::path(description);
    } catch (const std::exception& e) {
        qpdf_.reset();
        throw QpdfException(std::string("QPDF open failed: ") + e.what());
    }
}

void QpdfWrapper::saveToStream(std::FILE* file, bool linearize)
{
    if (!qpdf_) {
        if (file) std::fclose(file);
        throw QpdfException("Cannot save: no file is open");
    }
    if (!file) {
        throw QpdfException("QPDF save failed: null output stream");
    }

    try {
        // The description is diagnostic only — QPDFWriter never opens it.
        QPDFWriter writer(*qpdf_, "<staged output handle>", file,
                          /*close_file=*/true);
        if (linearize) {
            writer.setLinearization(true);
        }
        writer.setStaticID(false);
        writer.write();
    } catch (const std::exception& e) {
        throw QpdfException(std::string("QPDF save failed: ") + e.what());
    }
}

void QpdfWrapper::savePdfAToStream(std::FILE* file, bool disableObjectStreams)
{
    if (!qpdf_) {
        if (file) std::fclose(file);
        throw QpdfException("Cannot save: no file is open");
    }
    if (!file) {
        throw QpdfException("QPDF save failed: null output stream");
    }

    try {
        // The description is diagnostic only — QPDFWriter never opens it.
        QPDFWriter writer(*qpdf_, "<staged output handle>", file,
                          /*close_file=*/true);
        writer.setPreserveEncryption(false);
        writer.setObjectStreamMode(disableObjectStreams ? qpdf_o_disable
                                                        : qpdf_o_generate);
        writer.setLinearization(false);
        writer.setStaticID(false);
        writer.write();
    } catch (const std::exception& e) {
        throw QpdfException(std::string("QPDF save failed: ") + e.what());
    }
}

void QpdfWrapper::save(const std::filesystem::path& path, bool linearize)
{
    if (!qpdf_) {
        throw QpdfException("Cannot save: no file is open");
    }

    try {
        // QPDFWriter stores the filename pointer; the std::string returned
        // by path.string() must outlive the writer or .write() dereferences
        // a dangling buffer (caught by ASAN as heap-use-after-free).
        const std::string pathStr = path.string();
        QPDFWriter writer(*qpdf_, pathStr.c_str());
        if (linearize) {
            writer.setLinearization(true);
        }
        writer.setStaticID(false);
        writer.write();
    } catch (const std::exception& e) {
        throw QpdfException(std::string("QPDF save failed: ") + e.what());
    }
}

void QpdfWrapper::saveIncremental(const std::filesystem::path& path)
{
    if (!qpdf_) {
        throw QpdfException("Cannot save incrementally: no file is open");
    }

    try {
        const std::string pathStr = path.string();
        QPDFWriter writer(*qpdf_, pathStr.c_str());
        writer.setQDFMode(false);
        writer.setNewlineBeforeEndstream(false);
        writer.write();
    } catch (const std::exception& e) {
        throw QpdfException(std::string("QPDF incremental save failed: ") + e.what());
    }
}

StructureCheck QpdfWrapper::checkStructure() const
{
    StructureCheck result;
    if (!qpdf_) {
        result.problems.push_back("no file is open");
        return result;
    }

    // getAllPages / getAllObjects mutate internal caches, so a non-const handle
    // is required even though the check is logically const.
    QPDF& q = const_cast<QPDF&>(*qpdf_);

    // 1. Walk the full page tree. This forces resolution of every page dict and
    //    its inherited attributes; a broken /Pages node or dangling page ref
    //    throws here.
    try {
        QPDFPageDocumentHelper helper(q);
        auto pages = helper.getAllPages();
        for (auto& page : pages) {
            // Touching the keys forces the page object to be read from the file.
            (void)page.getObjectHandle().getKeys();
        }
    } catch (const std::exception& e) {
        result.problems.push_back(std::string("page-tree walk failed: ") + e.what());
    }

    // 2. Resolve every indirect object in the xref table. getAllObjects()
    //    fixes dangling references and reads each object; unparsing a
    //    dictionary/stream forces its nested references to resolve too. A
    //    corrupt object or unreadable stream dict surfaces here.
    try {
        auto objs = q.getAllObjects();
        for (auto& obj : objs) {
            try {
                if (obj.isStream()) {
                    // A stream's keys live on its dictionary; calling getKeys()
                    // directly on the stream handle trips QPDF's type warning.
                    (void)obj.getDict().getKeys();
                } else if (obj.isDictionary()) {
                    (void)obj.getKeys();
                } else {
                    (void)obj.getTypeCode();
                }
            } catch (const std::exception& e) {
                result.problems.push_back(
                    std::string("object resolution failed: ") + e.what());
            }
        }
    } catch (const std::exception& e) {
        result.problems.push_back(std::string("object-graph walk failed: ") + e.what());
    }

    // 3. Linearization sanity checks (only meaningful for linearized files).
    try {
        if (q.isLinearized()) {
            if (!q.checkLinearization()) {
                result.problems.push_back("linearization check reported errors/warnings");
            }
        }
    } catch (const std::exception& e) {
        result.problems.push_back(std::string("linearization check failed: ") + e.what());
    }

    // 4. Fold in any QPDF-level warnings accumulated during the walk (e.g. a
    //    damaged xref that QPDF silently recovered). getWarnings() drains them.
    try {
        for (auto& w : q.getWarnings()) {
            result.problems.push_back(std::string("qpdf warning: ") + w.what());
        }
    } catch (const std::exception& e) {
        result.problems.push_back(std::string("warning retrieval failed: ") + e.what());
    }

    result.ok = result.problems.empty();
    return result;
}

StructureCheck QpdfWrapper::checkFile(const std::filesystem::path& path,
                                      const std::string& password)
{
    QpdfWrapper wrapper;
    try {
        wrapper.open(path, password);
    } catch (const std::exception& e) {
        StructureCheck result;
        result.problems.push_back(std::string("open failed: ") + e.what());
        return result;
    }
    return wrapper.checkStructure();
}

int QpdfWrapper::pageCount() const
{
    if (!qpdf_) {
        throw QpdfException("Cannot get page count: no file is open");
    }

    // QPDFPageDocumentHelper needs non-const QPDF, but pageCount is logically const
    auto& mutableQpdf = const_cast<QPDF&>(*qpdf_);
    QPDFPageDocumentHelper helper(mutableQpdf);
    return static_cast<int>(helper.getAllPages().size());
}

} // namespace pdfe::core
