#include <Core/File.hpp>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Sleak {

    File::File() : m_mode("") {}

    File::~File() {
        Close();
    }

    bool File::Open(const std::string& filename, const std::string& mode) {
        Close(); // Close any previously opened file

        // Set the file stream mode based on the input mode string
        std::ios_base::openmode streamMode = std::ios_base::in; // Default to input mode
        if (mode.find('w') != std::string::npos) {
            streamMode = std::ios_base::out;
        }
        if (mode.find('b') != std::string::npos) {
            streamMode |= std::ios_base::binary;
        }

        // Open the file stream
        m_fileStream.open(filename, streamMode);
        if (!m_fileStream.is_open()) {
            std::cerr << "Failed to open file: " << filename << std::endl;
            return false;
        }

        m_mode = mode;
        return true;
    }

    void File::Close() {
        if (m_fileStream.is_open()) {
            m_fileStream.close();
        }
    }

    std::string File::ReadAllText() {
        if (!m_fileStream.is_open()) {
            std::cerr << "File is not open for reading." << std::endl;
            return "";
        }

        std::stringstream buffer;
        buffer << m_fileStream.rdbuf();
        return buffer.str();
    }

    std::vector<unsigned char> File::ReadAllBytes() {
        if (!m_fileStream.is_open()) {
            std::cerr << "File is not open for reading." << std::endl;
            return {};
        }

        // Move the file pointer to the end to determine the file size
        m_fileStream.seekg(0, std::ios::end);
        std::streampos fileSize = m_fileStream.tellg();
        m_fileStream.seekg(0, std::ios::beg);

        // Read the file into a vector
        std::vector<unsigned char> data(fileSize);
        m_fileStream.read(reinterpret_cast<char*>(data.data()), fileSize);
        return data;
    }

    bool File::WriteAllText(const std::string& content) {
        if (!m_fileStream.is_open()) {
            std::cerr << "File is not open for writing." << std::endl;
            return false;
        }

        m_fileStream << content;
        return m_fileStream.good();
    }

    bool File::WriteAllBytes(const std::vector<unsigned char>& data) {
        if (!m_fileStream.is_open()) {
            std::cerr << "File is not open for writing." << std::endl;
            return false;
        }

        m_fileStream.write(reinterpret_cast<const char*>(data.data()), data.size());
        return m_fileStream.good();
    }

    bool File::Exists(const std::string& filename) {
        return ExistsImpl(filename);
    }

    // Platform-specific implementation for file existence check
    bool File::ExistsImpl(const std::string& filename) {
#ifdef _WIN32
        DWORD attribs = GetFileAttributesA(filename.c_str());
        return (attribs != INVALID_FILE_ATTRIBUTES && !(attribs & FILE_ATTRIBUTE_DIRECTORY));
#else
        struct stat buffer;
        return (stat(filename.c_str(), &buffer) == 0);
#endif
    }

} // namespace Sleak