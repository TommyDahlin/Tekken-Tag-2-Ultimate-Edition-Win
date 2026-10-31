#include "cafe/vfs.h"
#include "cafe/runtime.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <cstring>
namespace fs=std::filesystem;
namespace cafe { [[noreturn]] void fatal(const char* text,...) { throw std::runtime_error(text); } }
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    using namespace cafe::vfs;
    auto root=fs::current_path()/ ("windows-vfs-smoke-"+std::to_string(GetCurrentProcessId()));
    require(fs::create_directory(root),"create test root");
    try {
        fs::create_directories(root/"game/content");
        const char bytes[]={'\r','\n','\x1a','\0','\xff'};
        { std::ofstream out(root/"game/content/Binary.bin",std::ios::binary);out.write(bytes,sizeof(bytes)); }
        open_game(root/"game");set_save_root(root/"save");
        auto data=read_whole("/vol/CONTENT/binary.BIN");
        require(data && data->size()==sizeof(bytes) && !std::memcmp(data->data(),bytes,sizeof(bytes)),"case-insensitive binary game read");
        int32_t handle=0;
        require(open_file("/vol/content/Binary.bin","w",handle)==kPermissionError,"game data stays read-only");
        require(open_file("/vol/save/profile.bin","w+",handle)==kOk,"save create");
        uint64_t count=0;
        require(write_file(handle,bytes,sizeof(bytes),0,count)==kOk && count==sizeof(bytes),"save write");
        char got[sizeof(bytes)]{};
        require(read_file(handle,got,sizeof(got),0,count)==kOk && count==sizeof(got) && !std::memcmp(got,bytes,sizeof(bytes)),"save read");
        require(flush_file(handle)==kOk && close_file(handle)==kOk,"save flush/close");
        if(argc==2) {
            open_game(argv[1]);
            auto rpx=read_whole("/vol/code/Tekken.rpx");
            require(rpx && rpx->size()>1000000 && (*rpx)[0]==0x7f && (*rpx)[1]=='E',"real Wii U executable read through runtime VFS");
            std::printf("Runtime VFS read real RPX: %zu bytes\n",rpx->size());
        }
        fs::remove_all(root);
        std::puts("Windows VFS game reads and binary save round trip passed");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what());fs::remove_all(root);return 1; }
}
