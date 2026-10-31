#pragma once
// Installer adapters share the runtime's Unicode and synchronized file I/O.
#include "file_io.h"
#include <cstdlib>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace cafe::host::file {
inline std::filesystem::path home() {
    const char* p = std::getenv("HOME");
#ifdef _WIN32
    if (!p || !*p) p = std::getenv("USERPROFILE");
#endif
    return p && *p ? std::filesystem::path(p) : std::filesystem::path("/");
}
using io::open;
inline auto read_at(int fd, void* p, size_t n, uint64_t offset) { return io::pread(fd, p, n, offset); }
inline int sync_data(int fd) { return io::flush(fd); }
inline void replace(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        ec = std::error_code(GetLastError(), std::system_category());
    else ec.clear();
#else
    std::filesystem::rename(from, to, ec);
#endif
}
inline std::filesystem::path executable() {
#ifdef _WIN32
    std::wstring p(32768, L'\0');
    auto n = GetModuleFileNameW(nullptr, p.data(), p.size());
    if (!n || n >= p.size()) throw std::runtime_error("cannot locate executable");
    p.resize(n); return p;
#else
    return std::filesystem::read_symlink("/proc/self/exe");
#endif
}
}
