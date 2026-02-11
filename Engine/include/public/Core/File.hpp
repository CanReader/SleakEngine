#ifndef _FILE_H_
#define _FILE_H_

#include <string>
#include <vector>
#include <fstream>

namespace Sleak {

    class File {
    public:
        // Constructor and Destructor
        File();
        ~File();

        // Opens a file with the specified mode
        bool Open(const std::string& filename, const std::string& mode);

        // Closes the currently open file
        void Close();

        // Reads all text from the file
        std::string ReadAllText();

        // Reads all bytes from the file
        std::vector<unsigned char> ReadAllBytes();

        // Writes all text to the file
        bool WriteAllText(const std::string& content);

        // Writes all bytes to the file
        bool WriteAllBytes(const std::vector<unsigned char>& data);

        // Checks if a file exists
        static bool Exists(const std::string& filename);

    private:
        // File stream for reading and writing
        std::fstream m_fileStream;

        // Current file mode
        std::string m_mode;

        // Platform-specific implementation for file existence check
        static bool ExistsImpl(const std::string& filename);
    };

} // namespace Sleak

#endif // _FILE_H_