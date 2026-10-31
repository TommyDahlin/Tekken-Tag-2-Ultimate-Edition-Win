// Native Windows entry point for the Windows-only launcher frontend.
// No GTK, POSIX process code, game assets, or user-specific paths are embedded.
//
// As CMake builds it, it opens the launcher of the project it was built in.
// The release's Ultimate_Launcher-x86_64.exe is this program with the
// project's files in it (make_launcher_exe.py): it keeps them in
// %LOCALAPPDATA%\TTT2Ultimate\launcher (files_folder), unpacked again whenever they differ
// from the last ones, and opens the launcher from there, which can replace
// this .exe with a newer release (self_update.py).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace fs = std::filesystem;

#ifndef TTT2_LAUNCHER_VERSION
#error "TTT2_LAUNCHER_VERSION must name the launcher's version (launcher/builder/CMakeLists.txt)"
#endif

// The files: for each, its path (UTF-8, '/' between folders) after its
// length (4 bytes), its size (8 bytes) and its bytes; then this trailer. All
// numbers little-endian. make_launcher_exe.py writes it as resource
// kFilesResource (or appends it to a launcher built already, for tests);
// self_update.py checks that a download has it.
struct Trailer {
    uint64_t size, count;     // of the files' part, and how many files
    unsigned char sha256[32]; // of the files' part
    char magic[8];
};
static_assert(sizeof(Trailer)==56);
constexpr char kMagic[8]={'T','T','T','2','P','A','Y','1'};
constexpr int kFilesResource=100; // (RCDATA)

std::wstring quote(const std::wstring& s) {
    std::wstring out=L"\""; size_t slashes=0;
    for(wchar_t c:s) {
        if(c==L'\\') { ++slashes; continue; }
        if(c==L'\"') out.append(slashes*2+1,L'\\'); else out.append(slashes,L'\\');
        slashes=0; out+=c;
    }
    out.append(slashes*2,L'\\'); return out+L'\"';
}
std::wstring environment(const wchar_t* name) {
    DWORD n=GetEnvironmentVariableW(name,nullptr,0); if(!n) return {};
    std::wstring result(n,L'\0'); GetEnvironmentVariableW(name,result.data(),n); result.resize(n-1); return result;
}
// Where the release keeps its files: %LOCALAPPDATA%\TTT2Ultimate\launcher, unless
// that path has letters beyond ASCII (a user name such as José): the game is
// built there, and the batch files CMake writes in UTF-8 lose such letters
// (cmd.exe reads them in the console's code page). Then %PUBLIC%'s, else the
// system drive's.
fs::path files_folder() {
    const auto data=environment(L"LOCALAPPDATA");
    if(data.empty()) throw std::runtime_error("LOCALAPPDATA is not set.");
    const auto drive=environment(L"SystemDrive");
    for(const auto& base:{data,environment(L"PUBLIC"),drive.empty() ? drive : drive+L"\\"}) {
        if(!base.empty() && std::all_of(base.begin(),base.end(),[](wchar_t c) { return c<128; }))
            return fs::path(base)/L"TTT2Ultimate\\launcher";
    }
    return fs::path(data)/L"TTT2Ultimate\\launcher";
}
std::wstring wide(std::string_view s) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0);
    std::wstring out(n,L'\0'); MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),n); return out;
}
std::string narrow(const std::wstring& s) {
    if(s.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    std::string out(n,'\0'); WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),n,nullptr,nullptr); return out;
}
fs::path python(const fs::path& folder) {
    if(auto value=environment(L"TTT2_PYTHON"); !value.empty()) {
        if(!fs::is_regular_file(value)) throw std::runtime_error("TTT2_PYTHON does not name a Python executable.");
        return value;
    }
    const auto portable=folder.parent_path()/"local/windows-tools/python-runtime/python/python.exe";
    if(fs::is_regular_file(portable)) return portable;
    for(HKEY hive:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE}) {
        HKEY parent;
        if(RegOpenKeyExW(hive,L"Software\\Python\\PythonCore",0,KEY_READ,&parent)!=ERROR_SUCCESS) continue;
        wchar_t version[256]; DWORD length;
        std::vector<std::wstring> versions;
        for(DWORD i=0;;++i) {
            length=256;
            if(RegEnumKeyExW(parent,i,version,&length,nullptr,nullptr,nullptr,nullptr)!=ERROR_SUCCESS) break;
            if(version[0]==L'3') versions.emplace_back(version);
        }
        for(auto it=versions.rbegin();it!=versions.rend();++it) {
            HKEY key;
            if(RegOpenKeyExW(parent,(*it+L"\\InstallPath").c_str(),0,KEY_READ,&key)!=ERROR_SUCCESS) continue;
            wchar_t path[32768]; DWORD bytes=sizeof(path),type=0;
            const auto result=RegQueryValueExW(key,L"ExecutablePath",nullptr,&type,reinterpret_cast<BYTE*>(path),&bytes);
            RegCloseKey(key);
            if(result==ERROR_SUCCESS && type==REG_SZ && fs::is_regular_file(path)) { RegCloseKey(parent); return path; }
        }
        RegCloseKey(parent);
    }
    wchar_t found[32768];
    if(SearchPathW(nullptr,L"python.exe",nullptr,32768,found,nullptr) && fs::is_regular_file(found)) return found;
    throw std::runtime_error("Install Python 3 with Tcl/Tk from python.org, or set TTT2_PYTHON to its python.exe.");
}

// ---- The release .exe: its files.

std::string read_file(const fs::path& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{}; std::string out;
    if(GetFileSizeEx(file,&size) && size.QuadPart<(int64_t(1)<<31)) {
        out.resize(static_cast<size_t>(size.QuadPart)); DWORD n=0;
        if(!out.empty() && (!ReadFile(file,out.data(),static_cast<DWORD>(out.size()),&n,nullptr) || n!=out.size())) out.clear();
    }
    CloseHandle(file); return out;
}
// Written beside the file, then renamed over it: never half a file.
void write_file(const fs::path& path, std::string_view data) {
    const fs::path part=path.wstring()+L".part";
    HANDLE file=CreateFileW(part.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot write "+narrow(part.wstring())+".");
    bool ok=true;
    while(ok && !data.empty()) {
        DWORD n=static_cast<DWORD>(std::min<size_t>(data.size(),1<<20)),written=0;
        ok=WriteFile(file,data.data(),n,&written,nullptr) && written==n; data.remove_prefix(n);
    }
    ok=CloseHandle(file) && ok;
    if(!ok || !MoveFileExW(part.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(part.c_str()); throw std::runtime_error("Cannot write "+narrow(path.wstring())+".");
    }
}
std::string sha256(std::string_view data) {
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr; unsigned char digest[32];
    bool ok=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0 &&
            BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    while(ok && !data.empty()) {
        ULONG n=static_cast<ULONG>(std::min<size_t>(data.size(),1<<20));
        ok=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),n,0)>=0; data.remove_prefix(n);
    }
    ok=ok && BCryptFinishHash(hash,digest,sizeof digest,0)>=0;
    if(hash) BCryptDestroyHash(hash);
    if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    if(!ok) throw std::runtime_error("Cannot compute a SHA-256 on this PC.");
    return std::string(reinterpret_cast<char*>(digest),sizeof digest);
}
std::string hex(std::string_view bytes) {
    static const char digits[]="0123456789abcdef"; std::string out;
    for(unsigned char c:bytes) { out+=digits[c>>4]; out+=digits[c&15]; }
    return out;
}

struct Payload { std::string files; Trailer trailer{}; };
// The files in this program (its resource, else appended to `exe`), checked;
// none when it has none (a CMake build).
Payload payload(const fs::path& exe) {
    Payload p; std::string all;
    if(HRSRC found=FindResourceW(nullptr,MAKEINTRESOURCEW(kFilesResource),MAKEINTRESOURCEW(10))) {
        HGLOBAL loaded=LoadResource(nullptr,found); const DWORD n=SizeofResource(nullptr,found);
        if(const void* data=loaded ? LockResource(loaded) : nullptr; data && n) all.assign(static_cast<const char*>(data),n);
    }
    if(all.empty()) all=read_file(exe);
    if(all.size()<sizeof(Trailer)) throw std::runtime_error("Cannot read Ultimate Launcher's own file.");
    std::memcpy(&p.trailer,all.data()+all.size()-sizeof(Trailer),sizeof(Trailer));
    if(std::memcmp(p.trailer.magic,kMagic,sizeof kMagic)) return {};
    if(p.trailer.size>all.size()-sizeof(Trailer)) throw std::runtime_error("This Ultimate Launcher is incomplete: download it again.");
    p.files=all.substr(all.size()-sizeof(Trailer)-p.trailer.size,p.trailer.size);
    if(sha256(p.files)!=std::string(reinterpret_cast<char*>(p.trailer.sha256),32))
        throw std::runtime_error("This Ultimate Launcher is damaged: download it again.");
    return p;
}

struct Entry { std::string path; std::string_view data; };
std::vector<Entry> entries(const Payload& p) {
    std::vector<Entry> out; std::string_view rest=p.files;
    auto take=[&](size_t n) {
        if(n>rest.size()) throw std::runtime_error("This Ultimate Launcher is damaged: download it again.");
        auto part=rest.substr(0,n); rest.remove_prefix(n); return part;
    };
    auto number=[&](size_t n) { uint64_t v=0; std::memcpy(&v,take(n).data(),n); return v; };
    for(uint64_t i=0;i<p.trailer.count;++i) {
        std::string path(take(number(4))); auto data=take(number(8));
        // Only plain paths inside the folder: no "..", drive, or backslash.
        bool safe=!path.empty() && path.front()!='/' && path.find_first_of("\\:")==std::string::npos;
        for(size_t start=0;safe && start<=path.size();) {
            size_t end=std::min(path.find('/',start),path.size());
            auto part=std::string_view(path).substr(start,end-start);
            safe=!part.empty() && part!="." && part!=".."; start=end+1;
        }
        if(!safe) throw std::runtime_error("This Ultimate Launcher holds an unsafe path: "+path);
        out.push_back({std::move(path),data});
    }
    if(!rest.empty()) throw std::runtime_error("This Ultimate Launcher is damaged: download it again.");
    return out;
}

// The files into `root`, when they are not the ones unpacked there last
// (local/launcher-files.txt: their SHA-256, then their paths). Files only
// the last ones had are deleted; nothing else there (local/, the build, the
// generated code) is touched.
void unpack(const Payload& p, const fs::path& root) {
    const fs::path stamp=root/"local/launcher-files.txt";
    const std::string digest=hex(std::string_view(reinterpret_cast<const char*>(p.trailer.sha256),32));
    std::vector<std::string> last; std::string text=read_file(stamp);
    for(size_t start=0;start<text.size();) {
        size_t end=std::min(text.find('\n',start),text.size());
        last.push_back(text.substr(start,end-start)); start=end+1;
    }
    if(!last.empty() && last[0]==digest && fs::is_regular_file(root/"launcher/windows/main.py")) return;
    std::set<std::string> now; std::string list=digest+"\n";
    for(const auto& e:entries(p)) {
        const fs::path path=root/fs::path(wide(e.path));
        fs::create_directories(path.parent_path());
        write_file(path,e.data); now.insert(e.path); list+=e.path+"\n";
    }
    for(size_t i=1;i<last.size();++i) {
        if(last[i].empty() || now.count(last[i]) || last[i].find("..")!=std::string::npos) continue;
        DeleteFileW((root/fs::path(wide(last[i]))).c_str());
    }
    fs::create_directories(stamp.parent_path()); write_file(stamp,list);
}

// The launcher an update replaces ends first: its window would be shown
// instead of this one's, and its files are about to be replaced.
void wait_for_replaced() {
    const auto pid=environment(L"TTT2_LAUNCHER_AFTER");
    if(pid.empty()) return;
    SetEnvironmentVariableW(L"TTT2_LAUNCHER_AFTER",nullptr);
    if(HANDLE old=OpenProcess(SYNCHRONIZE,FALSE,std::wcstoul(pid.c_str(),nullptr,10))) {
        WaitForSingleObject(old,60000); CloseHandle(old);
    }
}

// The private Python of the launcher's files, as "Start Ultimate Launcher.cmd"
// gets it ready (downloaded and checked) in a console window of its own.
fs::path release_python(const fs::path& root) {
    if(!environment(L"TTT2_PYTHON").empty()) return python(root);
    const fs::path interpreter=root/"local/windows-tools/python-runtime/python/python.exe";
    if(fs::is_regular_file(interpreter)) return interpreter;
    std::wstring shell=environment(L"ComSpec"); if(shell.empty()) shell=L"cmd.exe";
    std::wstring command=L"cmd.exe /d /c \""+quote((root/L"Start Ultimate Launcher.cmd").wstring())+L" --check\"";
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION process{};
    if(!CreateProcessW(shell.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NEW_CONSOLE,nullptr,root.c_str(),&startup,&process))
        throw std::runtime_error("Cannot start the Windows command processor to get Python ready.");
    CloseHandle(process.hThread); WaitForSingleObject(process.hProcess,INFINITE);
    DWORD result=1; GetExitCodeProcess(process.hProcess,&result); CloseHandle(process.hProcess);
    if(result || !fs::is_regular_file(interpreter))
        throw std::runtime_error("Ultimate Launcher could not get its Python ready. Check your internet connection and start it again. "
                                 "Details: "+narrow((root/L"local/windows-bootstrap.log").wstring()));
    return interpreter;
}

void say(std::string_view text, DWORD where=STD_OUTPUT_HANDLE) {
    HANDLE out=GetStdHandle(where); DWORD n=0;
    if(out && out!=INVALID_HANDLE_VALUE) WriteFile(out,text.data(),static_cast<DWORD>(text.size()),&n,nullptr);
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    bool quiet=false; // (asked by a program: errors to standard error, not a window)
    try {
        int argc=0; LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        if(!argv) throw std::runtime_error("Cannot read launcher arguments.");
        std::vector<std::wstring> args(argv+1,argv+argc); LocalFree(argv);
        quiet=!args.empty() && (args[0]==L"--version" || args[0]==L"--unpack");
        // (Asked by an update, before it puts this .exe in place.)
        if(!args.empty() && args[0]==L"--version") { say("ttt2-launcher " TTT2_LAUNCHER_VERSION "\n"); return 0; }
        wchar_t self[32768]; DWORD n=GetModuleFileNameW(nullptr,self,32768);
        if(!n || n>=32768) throw std::runtime_error("Cannot locate Ultimate Launcher.");
        const fs::path exe=self;
        const Payload files=payload(exe);
        const bool release=std::memcmp(files.trailer.magic,kMagic,sizeof kMagic)==0;
        // (Tests: the files into a folder, and nothing else.)
        if(args.size()==2 && args[0]==L"--unpack") {
            if(!release) throw std::runtime_error("This launcher holds no files.");
            unpack(files,args[1]); return 0;
        }
        fs::path folder=exe.parent_path(),script,build=folder,logfile=folder/"launcher.log",interpreter;
        if(release) {
            wait_for_replaced();
            DeleteFileW((folder/(exe.stem().wstring()+L".old.exe")).c_str()); // (what an update replaced)
            folder=files_folder();
            // (Started twice at once: one unpacks, the other then finds the files there.)
            HANDLE lock=CreateMutexW(nullptr,FALSE,L"Local\\TTT2UltimateLauncherFiles");
            if(lock) WaitForSingleObject(lock,INFINITE);
            try { unpack(files,folder); } catch(...) { if(lock) { ReleaseMutex(lock); CloseHandle(lock); } throw; }
            if(lock) { ReleaseMutex(lock); CloseHandle(lock); }
            script=fs::path(folder/"launcher/windows/main.py").make_preferred(); build=folder/"build-windows-tools";
            logfile=fs::path(folder/"local/launcher.log").make_preferred();
            interpreter=release_python(folder);
            SetEnvironmentVariableW(L"TTT2_LAUNCHER_EXE",exe.c_str());
        } else {
            script=folder.parent_path()/"launcher/windows/main.py";
            if(!fs::exists(script)) script=folder/"launcher/windows/main.py";
            if(!fs::exists(script)) throw std::runtime_error("Keep this launcher in the project's build folder, beside the launcher/windows source folder.");
            interpreter=python(folder);
        }
        bool check=false;
        std::wstring command=quote(interpreter.wstring())+L" -u "+quote(script.wstring())+L" --build-dir "+quote(build.wstring());
        for(const auto& arg:args) { command+=L" "+quote(arg); if(arg==L"--check") check=true; }
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
        HANDLE log=CreateFileW(logfile.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(log==INVALID_HANDLE_VALUE || input==INVALID_HANDLE_VALUE) {
            if(log!=INVALID_HANDLE_VALUE) CloseHandle(log); if(input!=INVALID_HANDLE_VALUE) CloseHandle(input);
            throw std::runtime_error("Cannot write "+narrow(logfile.wstring())+".");
        }
        STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdInput=input; startup.hStdOutput=log; startup.hStdError=log;
        PROCESS_INFORMATION process{};
        bool ok=CreateProcessW(interpreter.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,folder.c_str(),&startup,&process);
        CloseHandle(log); CloseHandle(input);
        if(!ok) throw std::runtime_error("Cannot start Python. See "+narrow(logfile.wstring())+".");
        CloseHandle(process.hThread);
        // Report early startup failures, then release the bootstrap executable
        // so Build/Rebuild (or an update of the release .exe) can replace it
        // while the Python window stays open.
        if(WaitForSingleObject(process.hProcess,check ? INFINITE : 2500)==WAIT_TIMEOUT) {
            CloseHandle(process.hProcess); return 0;
        }
        DWORD result=1;
        GetExitCodeProcess(process.hProcess,&result); CloseHandle(process.hProcess);
        if(result && !check) {
            const std::wstring text=L"Ultimate Launcher stopped with an error. Read "+logfile.wstring()+
                (release ? L"." : L". Python needs Tcl/Tk installed.");
            MessageBoxW(nullptr,text.c_str(),L"Ultimate Launcher",MB_OK|MB_ICONERROR);
        }
        return static_cast<int>(result);
    } catch(const std::exception& error) {
        if(quiet) say(std::string("ttt2-launcher: ")+error.what()+"\n",STD_ERROR_HANDLE);
        else MessageBoxW(nullptr,wide(error.what()).c_str(),L"Ultimate Launcher",MB_OK|MB_ICONERROR);
        return 1;
    }
}
