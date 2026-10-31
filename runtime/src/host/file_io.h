#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
namespace cafe::host::io {
#ifdef _WIN32
using Stat = struct _stat64;
inline int stat(const std::filesystem::path& p, Stat* s) { return _wstat64(p.c_str(), s); }
inline int fstat(int fd, Stat* s) { return _fstat64(fd, s); }
inline bool is_directory(const Stat& s) { return (s.st_mode & _S_IFMT) == _S_IFDIR; }
inline int open(const std::filesystem::path& p, int flags, int mode = 0644) {
    return _wopen(p.c_str(), flags | _O_BINARY | _O_NOINHERIT, mode);
}
inline int close(int fd) { return _close(fd); }
inline int flush(int fd) { return _commit(fd); }
// CRT positional I/O is protected across seek+transfer. All callers use this
// wrapper, including reads made outside the VFS lock. Offsets stay 64-bit.
inline std::mutex position_mutex;
inline int64_t pread(int fd, void* buffer, uint64_t size, uint64_t at) {
    std::lock_guard lock(position_mutex);
    if (at > INT64_MAX) { errno = EINVAL; return -1; }
    if (_lseeki64(fd, static_cast<int64_t>(at), SEEK_SET) < 0) return -1;
    return _read(fd, buffer, static_cast<unsigned>(std::min<uint64_t>(size, INT_MAX)));
}
inline int64_t pwrite(int fd, const void* buffer, uint64_t size, uint64_t at) {
    std::lock_guard lock(position_mutex);
    if (at > INT64_MAX) { errno = EINVAL; return -1; }
    if (_lseeki64(fd, static_cast<int64_t>(at), SEEK_SET) < 0) return -1;
    return _write(fd, buffer, static_cast<unsigned>(std::min<uint64_t>(size, INT_MAX)));
}
#else
using Stat = struct stat;
inline int stat(const std::filesystem::path& p, Stat* s) { return ::stat(p.c_str(), s); }
inline int fstat(int fd, Stat* s) { return ::fstat(fd, s); }
inline bool is_directory(const Stat& s) { return S_ISDIR(s.st_mode); }
inline int open(const std::filesystem::path& p, int flags, int mode = 0644) { return ::open(p.c_str(), flags | O_CLOEXEC, mode); }
inline int close(int fd) { return ::close(fd); }
inline int flush(int fd) { return ::fsync(fd); }
inline int64_t pread(int fd, void* buffer, uint64_t size, uint64_t at) { return ::pread(fd, buffer, size, static_cast<off_t>(at)); }
inline int64_t pwrite(int fd, const void* buffer, uint64_t size, uint64_t at) { return ::pwrite(fd, buffer, size, static_cast<off_t>(at)); }
#endif
} // namespace cafe::host::io
