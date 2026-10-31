// Native Windows archive extraction. Existing files are never overwritten.
#include <zarchive/zarchivereader.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
namespace fs = std::filesystem;
namespace {
void validate_component(std::string_view name) {
    if(name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ')
        throw std::runtime_error("Unsafe Windows archive path");
    for(unsigned char c : name)
        if(c < 32 || c == 127 || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos)
            throw std::runtime_error("Unsafe Windows archive path");
    std::string stem(name.substr(0, name.find('.')));
    for(auto& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if(stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
       (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '0' && stem[3] <= '9'))
        throw std::runtime_error("Reserved Windows archive filename");
}
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if(value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
};
struct Root {
    fs::path path;
    std::vector<std::unique_ptr<Handle>> locks;
    void directory(const fs::path& p) {
        if(GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES && !CreateDirectoryW(p.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            throw std::runtime_error("Cannot create output directory");
        auto h = std::make_unique<Handle>(CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if(h->value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h->value, &info) ||
           !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Output path is not a plain directory");
        locks.push_back(std::move(h)); // No delete sharing: ancestors cannot be renamed during extraction.
    }
};
Root output_directory(const fs::path& output) {
    Root root;
    root.path = fs::absolute(output).lexically_normal();
    fs::path current = root.path.root_path();
    root.directory(current);
    for(const auto& part : root.path.relative_path()) { current /= part; root.directory(current); }
    return root;
}
struct Entry {
    std::string path;
    ZArchiveNodeHandle node;
    uint64_t size;
};

std::vector<Entry> inventory(ZArchiveReader& reader) {
    struct Directory { std::string path; ZArchiveNodeHandle node; unsigned depth; };
    const auto root = reader.LookUp("", false, true);
    if (root == ZARCHIVE_INVALID_NODE)
        throw std::runtime_error("Archive has no valid root directory");
    std::vector<Directory> pending{{"", root, 0}};
    std::unordered_set<ZArchiveNodeHandle> visited;
    std::unordered_set<std::string> paths;
    std::vector<Entry> files;
    while (!pending.empty()) {
        auto dir = std::move(pending.back());
        pending.pop_back();
        if (dir.depth > 256 || !visited.insert(dir.node).second)
            throw std::runtime_error("Archive has cyclic or excessively deep directories");
        const auto count = reader.GetDirEntryCount(dir.node);
        for (uint32_t i = 0; i < count; ++i) {
            ZArchiveReader::DirEntry entry{};
            if (!reader.GetDirEntry(dir.node, i, entry) || entry.isFile == entry.isDirectory)
                throw std::runtime_error("Invalid archive directory entry");
            validate_component(entry.name);
            std::string path = dir.path.empty() ? std::string(entry.name)
                                               : dir.path + "/" + std::string(entry.name);
            if (!paths.insert(path).second)
                throw std::runtime_error("Duplicate archive path: " + path);
            const auto node = reader.LookUp(path, entry.isFile, entry.isDirectory);
            if (node == ZARCHIVE_INVALID_NODE)
                throw std::runtime_error("Cannot look up archive entry: " + path);
            if (entry.isDirectory)
                pending.push_back({std::move(path), node, dir.depth + 1});
            else
                files.push_back({std::move(path), node, reader.GetFileSize(node)});
        }
    }
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
    return files;
}

bool analysis_file(const std::string& path) {
    // WUA paths are TITLEID_VERSION/code/... and TITLEID_VERSION/meta/....
    const auto first = path.find('/');
    if (first == std::string::npos) return false;
    const auto second = path.find('/', first + 1);
    if (second == std::string::npos) return false;
    const auto directory = path.substr(first + 1, second - first - 1);
    return directory == "code" || directory == "meta";
}

void extract_file(ZArchiveReader& reader, const Entry& entry, Root& root) {
    auto parent = root.path;
    const fs::path relative(entry.path);
    for(const auto& part : relative.parent_path()) { parent /= part; root.directory(parent); }
    const auto destination = parent / relative.filename();
    Handle file(CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if(file.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create output; existing files are preserved: " + entry.path);
    std::vector<unsigned char> buffer(1024 * 1024);
    for(uint64_t offset = 0; offset < entry.size;) {
        const auto length = static_cast<DWORD>(std::min<uint64_t>(buffer.size(), entry.size-offset));
        if(reader.ReadFromFile(entry.node, offset, length, buffer.data()) != length)
            throw std::runtime_error("Short archive read: " + entry.path);
        DWORD written = 0;
        if(!WriteFile(file.value, buffer.data(), length, &written, nullptr) || written != length)
            throw std::runtime_error("Short output write: " + entry.path);
        offset += length;
    }
}
} // namespace
int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "";
    const bool list = command == "list";
    const bool analysis = command == "extract-analysis";
    const bool all = command == "extract-all";
    if ((!list && !analysis && !all) || argc != (list ? 3 : 4)) {
        std::cerr << "Usage:\n  wua_extract list ARCHIVE\n"
                  << "  wua_extract extract-analysis ARCHIVE OUTPUT_DIR\n"
                  << "  wua_extract extract-all ARCHIVE OUTPUT_DIR\n";
        return 2;
    }
    try {
        std::unique_ptr<ZArchiveReader> reader(ZArchiveReader::OpenFromFile(argv[2]));
        if (!reader) throw std::runtime_error("Cannot open WUA/ZArchive: " + std::string(argv[2]));
        const auto files = inventory(*reader);
        if (list) {
            std::cout << "path\tsize_bytes\n";
            for (const auto& entry : files)
                std::cout << entry.path << '\t' << entry.size << '\n';
        } else {
            auto root = output_directory(argv[3]);
            uint64_t bytes = 0;
            size_t count = 0;
            for (const auto& entry : files) {
                if (analysis && !analysis_file(entry.path)) continue;
                extract_file(*reader, entry, root);
                bytes += entry.size;
                ++count;
                std::cout << entry.path << '\n';
            }
            std::cerr << "Extracted " << count << " files (" << bytes << " bytes)\n";
            if (analysis && count == 0)
                throw std::runtime_error("No TITLEID_VERSION/code or /meta files found");
        }
        if (!std::cout) throw std::runtime_error("Cannot write standard output");
    } catch (const std::exception& error) {
        std::cerr << "wua_extract: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
