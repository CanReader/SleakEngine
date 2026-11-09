#include <Core/FileWatcher.hpp>
#include <Core/Logger.hpp>
#include <Core/OSDef.hpp>

#if defined(PLATFORM_LINUX)
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <unordered_map>
#elif defined(PLATFORM_WIN)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Sleak {

namespace {
constexpr size_t EVENT_BUFFER_SIZE = 4096;
}

#if defined(PLATFORM_LINUX)

struct FileWatcher::Impl {
    int fd = -1;
    std::unordered_map<int, std::string> dirs;
};

FileWatcher::FileWatcher() : m_impl(std::make_unique<Impl>()) {
    m_impl->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_impl->fd < 0)
        SLEAK_ERROR("FileWatcher: inotify_init1 failed: {}",
                    std::strerror(errno));
}

FileWatcher::~FileWatcher() {
    if (m_impl->fd >= 0) close(m_impl->fd);
}

bool FileWatcher::AddDirectory(const std::string& dir) {
    if (m_impl->fd < 0) return false;
    const int wd = inotify_add_watch(m_impl->fd, dir.c_str(),
                                     IN_CLOSE_WRITE | IN_MOVED_TO);
    if (wd < 0) {
        SLEAK_ERROR("FileWatcher: can't watch {}: {}", dir,
                    std::strerror(errno));
        return false;
    }
    m_impl->dirs[wd] = dir;
    return true;
}

void FileWatcher::Poll(std::vector<std::string>& outChanged) {
    if (m_impl->fd < 0) return;
    alignas(inotify_event) char buf[EVENT_BUFFER_SIZE];
    for (;;) {
        const ssize_t len = read(m_impl->fd, buf, sizeof(buf));
        if (len <= 0) break;
        for (char* p = buf; p < buf + len;) {
            const auto* ev = reinterpret_cast<const inotify_event*>(p);
            p += sizeof(inotify_event) + ev->len;
            if (ev->len == 0) continue;
            const auto dir = m_impl->dirs.find(ev->wd);
            if (dir == m_impl->dirs.end()) continue;
            outChanged.push_back(dir->second + "/" + ev->name);
        }
    }
}

#elif defined(PLATFORM_WIN)

namespace {
constexpr DWORD WATCH_FILTER =
    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME;

struct WatchedDirectory {
    std::string path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped = {};
    alignas(DWORD) char buffer[EVENT_BUFFER_SIZE];
};

/// Queues the next asynchronous change read for a directory.
bool Arm(WatchedDirectory& dir) {
    ResetEvent(dir.overlapped.hEvent);
    return ReadDirectoryChangesW(dir.handle, dir.buffer, sizeof(dir.buffer),
                                 FALSE, WATCH_FILTER, nullptr, &dir.overlapped,
                                 nullptr) != 0;
}

/// Converts a counted UTF-16 file name to UTF-8.
std::string ToUtf8(const WCHAR* name, int length) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, name, length, nullptr, 0,
                                         nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, name, length, out.data(), size, nullptr,
                        nullptr);
    return out;
}
}  // namespace

struct FileWatcher::Impl {
    std::vector<std::unique_ptr<WatchedDirectory> > dirs;
};

FileWatcher::FileWatcher() : m_impl(std::make_unique<Impl>()) {}

FileWatcher::~FileWatcher() {
    for (auto& dir : m_impl->dirs) {
        DWORD bytes = 0;
        CancelIoEx(dir->handle, &dir->overlapped);
        GetOverlappedResult(dir->handle, &dir->overlapped, &bytes, TRUE);
        CloseHandle(dir->overlapped.hEvent);
        CloseHandle(dir->handle);
    }
}

bool FileWatcher::AddDirectory(const std::string& dir) {
    auto watched = std::make_unique<WatchedDirectory>();
    watched->path = dir;
    watched->handle =
        CreateFileA(dir.c_str(), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (watched->handle == INVALID_HANDLE_VALUE) {
        SLEAK_ERROR("FileWatcher: can't open {} (error {})", dir,
                    GetLastError());
        return false;
    }
    watched->overlapped.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!watched->overlapped.hEvent || !Arm(*watched)) {
        SLEAK_ERROR("FileWatcher: can't watch {} (error {})", dir,
                    GetLastError());
        if (watched->overlapped.hEvent) CloseHandle(watched->overlapped.hEvent);
        CloseHandle(watched->handle);
        return false;
    }
    m_impl->dirs.push_back(std::move(watched));
    return true;
}

void FileWatcher::Poll(std::vector<std::string>& outChanged) {
    for (auto& dir : m_impl->dirs) {
        DWORD bytes = 0;
        if (!GetOverlappedResult(dir->handle, &dir->overlapped, &bytes, FALSE))
            continue;
        for (DWORD offset = 0; bytes > 0;) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                dir->buffer + offset);
            if (info->Action != FILE_ACTION_REMOVED &&
                info->Action != FILE_ACTION_RENAMED_OLD_NAME) {
                const int length =
                    static_cast<int>(info->FileNameLength / sizeof(WCHAR));
                outChanged.push_back(dir->path + "/" +
                                     ToUtf8(info->FileName, length));
            }
            if (info->NextEntryOffset == 0) break;
            offset += info->NextEntryOffset;
        }
        Arm(*dir);
    }
}

#else

struct FileWatcher::Impl {};

FileWatcher::FileWatcher() : m_impl(std::make_unique<Impl>()) {}

FileWatcher::~FileWatcher() = default;

bool FileWatcher::AddDirectory(const std::string& dir) {
    SLEAK_WARN("FileWatcher: not supported on this platform ({})", dir);
    return false;
}

void FileWatcher::Poll(std::vector<std::string>& outChanged) {
    (void)outChanged;
}

#endif

}  // namespace Sleak
