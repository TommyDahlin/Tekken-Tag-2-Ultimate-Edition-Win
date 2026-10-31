#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include "windows_import_config.h"
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    SetEnvironmentVariableW(L"TTT2_HELPER_CC",kCompiler);
    std::wstring command=L"\""+std::wstring(kPython)+L"\" -u \""+std::wstring(kRoot)+L"\\tools\\characters\\import_windows.py\"";
    STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION process{};
    if(!CreateProcessW(kPython,command.data(),nullptr,nullptr,FALSE,CREATE_NEW_CONSOLE,nullptr,kRoot,&start,&process)) {
        MessageBoxW(nullptr,L"Could not start the fighter importer.",L"TTT2 Ultimate",MB_OK|MB_ICONERROR);return 1;
    }
    CloseHandle(process.hThread);CloseHandle(process.hProcess);return 0;
}
