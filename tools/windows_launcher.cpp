// Development launcher: original code only; no game assets or generated code.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
namespace fs=std::filesystem;
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR arguments,int) {
    try {
        wchar_t self[32768];
        const auto length=GetModuleFileNameW(nullptr,self,32768);
        if(!length || length==32768) throw std::runtime_error("Cannot locate launcher");
        const auto folder=fs::path(self).parent_path();
        std::ifstream config(folder/"ttt2-game.txt",std::ios::binary);
        std::string archive;std::getline(config,archive);
        if(!archive.empty() && archive.back()=='\r') archive.pop_back();
        const auto game=fs::path(std::u8string(reinterpret_cast<const char8_t*>(archive.data()),archive.size()));
        if(archive.empty() || !fs::exists(game)) throw std::runtime_error("Set the Wii U archive path in ttt2-game.txt beside this launcher.");
        if(std::wstring(arguments)==L"--check") return fs::exists(folder/"ttt2-ultimate.exe") ? 0 : 2;
        const auto root=folder.parent_path();
        const auto user=root/"local/windows-userdata";
        const auto save=root/"local/windows-save";
        fs::create_directories(user);fs::create_directories(save);
        SetEnvironmentVariableW(L"LOCALAPPDATA",user.c_str());
        SetEnvironmentVariableW(L"TTT2_SAVE_DIR",save.c_str());
        const auto executable=folder/"ttt2-ultimate.exe";
        std::wstring command=L"\""+executable.wstring()+L"\" \""+game.wstring()+L"\"";
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        const auto log=CreateFileW((folder/"launch.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        const auto input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(log==INVALID_HANDLE_VALUE || input==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open launch log");
        STARTUPINFOW start{};start.cb=sizeof(start);start.dwFlags=STARTF_USESTDHANDLES;
        start.hStdInput=input;start.hStdOutput=log;start.hStdError=log;
        PROCESS_INFORMATION process{};
        const bool ok=CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,folder.c_str(),&start,&process);
        CloseHandle(log);CloseHandle(input);
        if(!ok) throw std::runtime_error("Cannot start ttt2-ultimate.exe");
        CloseHandle(process.hThread);CloseHandle(process.hProcess);
        return 0;
    } catch(const std::exception& error) { MessageBoxA(nullptr,error.what(),"TTT2 Ultimate",MB_OK|MB_ICONERROR);return 1; }
}
