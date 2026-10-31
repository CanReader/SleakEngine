#ifndef _FILE_WATCHER_HPP_
#define _FILE_WATCHER_HPP_

#include <Core/OSDef.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Sleak {

/// Reports files saved inside watched directories. Not recursive.
/// Generic: shader, library and asset hot reload all build on it.
/// @ingroup core
class ENGINE_API FileWatcher {
   public:
    FileWatcher();
    ~FileWatcher();
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    /// Starts watching a directory. Returns false if it can't be watched.
    bool AddDirectory(const std::string& dir);

    /// Appends full paths of files saved since the last call. Never blocks.
    void Poll(std::vector<std::string>& outChanged);

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace Sleak

#endif
