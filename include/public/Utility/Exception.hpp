#ifndef _EXCEPTIONS_H_
#define _EXCEPTIONS_H_

#include <exception>
#include <string>

namespace Sleak {

    /// Root of every Sleak-specific exception; carries a plain message string.
    /// @ingroup utility
    class SleakException : public std::exception {
    public:
        SleakException(const std::string& message) : message_(message) {}
        virtual const char* what() const noexcept override { return message_.c_str(); }

    protected:
        std::string message_;
    };

    /// Thrown when a container index is out of range.
    /// @ingroup utility
    class IndexOutOfBoundsException : public SleakException {
    public:
        IndexOutOfBoundsException(const std::string& message = "Index out of bounds")
            : SleakException(message) {}
    };

    /// Thrown for invalid iterator use (dereferencing end(), stale iterators, etc.).
    /// @ingroup utility
    class InvalidIteratorException : public SleakException {
    public:
        InvalidIteratorException(const std::string& message = "Invalid iterator operation")
            : SleakException(message) {}
    };

    /// Thrown when a function receives an argument it can't work with.
    /// @ingroup utility
    class InvalidArgumentException : public SleakException {
        public:
            InvalidArgumentException(const std::string& message = "Invalid argument operation")
                : SleakException(message) {}
        };

    /// Thrown by operations that require at least one element (e.g. Stack::top on an empty stack).
    /// @ingroup utility
    class EmptyContainerException : public SleakException {
    public:
        EmptyContainerException(const std::string& message = "Container is empty")
            : SleakException(message) {}
    };

    /// Thrown when code dereferences or accesses through a null pointer.
    /// @ingroup utility
    class NullPointerException : public SleakException {
    public:
        NullPointerException(const std::string& message = "Null pointer access")
            : SleakException(message) {}
    };

    /// Thrown when no active camera is registered where one is required.
    /// @ingroup utility
    class CameraNotFound : public NullPointerException {
        public:
            CameraNotFound(const std::string& message = "No camera found in static list!")
                : NullPointerException(message) {}
        };

    /// Thrown on file read/write failures.
    /// @ingroup utility
    class FileIOException : public SleakException {
    public:
        FileIOException(const std::string& message = "File I/O error")
            : SleakException(message) {}
    };

    /// Thrown by stubbed-out code paths that haven't been implemented yet.
    /// @ingroup utility
    class NotImplementedException : public SleakException {
    public:
        NotImplementedException(const std::string& message = "Not implemented")
            : SleakException(message) {}
    };
}

#endif // _EXCEPTIONS_H_