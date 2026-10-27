#pragma once

#include <stdexcept>
#include <string>

namespace pdfe::core {

class PdfeException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class MuPdfException : public PdfeException {
public:
    using PdfeException::PdfeException;
};

class QpdfException : public PdfeException {
public:
    using PdfeException::PdfeException;
};

class IoException : public PdfeException {
public:
    using PdfeException::PdfeException;
};

} // namespace pdfe::core
