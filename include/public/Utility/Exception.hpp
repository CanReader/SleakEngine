#ifndef _EXCEPTIONS_H_
#define _EXCEPTIONS_H_

#include <exception>
#include <string>

namespace Sleak {

    // Base class for all Sleak exceptions
    class SleakException : public std::exception {
    public:
        SleakException(const std::string& message) : message_(message) {}
        virtual const char* what() const noexcept override { return message_.c_str(); }

    protected:
        std::string message_;
    };

    // Exception for invalid array or list indices
    class IndexOutOfBoundsException : public SleakException {
    public:
        IndexOutOfBoundsException(const std::string& message = "Index out of bounds")
            : SleakException(message) {}
    };

    // Exception for invalid iterator operations
    class InvalidIteratorException : public SleakException {
    public:
        InvalidIteratorException(const std::string& message = "Invalid iterator operation")
            : SleakException(message) {}
    };

    class InvalidArgumentException : public SleakException {
        public:
            InvalidArgumentException(const std::string& message = "Invalid argument operation")
                : SleakException(message) {}
        };

    // Exception for empty container operations (e.g., accessing top of an empty stack)
    class EmptyContainerException : public SleakException {
    public:
        EmptyContainerException(const std::string& message = "Container is empty")
            : SleakException(message) {}
    };

    // Exception for null pointer operations
    class NullPointerException : public SleakException {
    public:
        NullPointerException(const std::string& message = "Null pointer access")
            : SleakException(message) {}
    };

    class CameraNotFound : public NullPointerException {
        public:
            CameraNotFound(const std::string& message = "No camera found in static list!")
                : NullPointerException(message) {}
        };

    // Exception for file I/O operations
    class FileIOException : public SleakException {
    public:
        FileIOException(const std::string& message = "File I/O error")
            : SleakException(message) {}
    };

    // Exception for operations that are not yet implemented
    class NotImplementedException : public SleakException {
    public:
        NotImplementedException(const std::string& message = "Not implemented")
            : SleakException(message) {}
    };
}

#endif // _EXCEPTIONS_H_