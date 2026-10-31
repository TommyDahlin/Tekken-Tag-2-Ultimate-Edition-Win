#include "host/file_io.h"
#include "host/local_time.h"
#include "host/digest.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>
namespace fs = std::filesystem;
void require(bool ok, const char* what) { if(!ok) { std::fprintf(stderr,"FAILED: %s\n",what); std::exit(1); } }
__attribute__((noinline)) void* thread_main(void* arg) {
    volatile unsigned char stack[2 * 1024 * 1024];
    for(size_t i=0;i<sizeof(stack);i+=4096) stack[i]=42;
    *static_cast<bool*>(arg) = stack[sizeof(stack)-4096] == 42;
    return nullptr;
}
int main() {
    using namespace cafe::host;
    const uint8_t abc[]={'a','b','c'};
    const std::array<uint8_t,16> md5={0x90,0x01,0x50,0x98,0x3c,0xd2,0x4f,0xb0,0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72};
    require(digest<16>(abc)==md5,"MD5 known vector");
    const std::array<uint8_t,32> sha={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    require(digest<32>(abc)==sha,"SHA256 known vector");
    auto file = fs::current_path()/L"windows-io-\u6e2c\u8a66.tmp";
    int fd = io::open(file,O_RDWR|O_CREAT|O_EXCL);
    require(fd>=0,"Unicode create");
    DWORD returned=0;
    require(DeviceIoControl(reinterpret_cast<HANDLE>(_get_osfhandle(fd)),FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&returned,nullptr),"sparse file");
    const char bytes[]={'\r','\n','\x1a','\0','\xff'};
    const uint64_t at=(1ull<<32)+123;
    require(io::pwrite(fd,bytes,sizeof(bytes),at)==sizeof(bytes),"write above 4GiB");
    char read[sizeof(bytes)]{};
    require(io::pread(fd,read,sizeof(read),at)==sizeof(read) && !std::memcmp(bytes,read,sizeof(read)),"binary round trip");
    io::Stat info{};require(io::fstat(fd,&info)==0 && info.st_size==at+sizeof(bytes),"64-bit file size");
    std::atomic<bool> good=true;
    auto access=[&](uint64_t offset,char value) {
        for(int i=0;i<300;++i) {
            char got=0;
            if(io::pwrite(fd,&value,1,offset)!=1 || io::pread(fd,&got,1,offset)!=1 || got!=value) good=false;
        }
    };
    std::thread a(access,at+100,'a'),b(access,at+200,'b'); a.join();b.join();
    require(good,"concurrent positional I/O");
    require(io::flush(fd)==0 && io::close(fd)==0,"flush and close");
    require(fs::remove(file),"test cleanup");
    TIME_ZONE_INFORMATION utc_zone{};
    require(utc_offset_seconds(1705320000, &utc_zone)==0,"UTC offset");
    TIME_ZONE_INFORMATION eastern{};
    eastern.Bias=300; eastern.DaylightBias=-60;
    eastern.StandardDate={0,11,0,1,2,0,0,0}; // first Sunday in November, 02:00
    eastern.DaylightDate={0,3,0,2,2,0,0,0}; // second Sunday in March, 02:00
    require(utc_offset_seconds(1705320000, &eastern)==-18000,"winter offset");
    require(utc_offset_seconds(1721044800, &eastern)==-14400,"summer offset");
    TIME_ZONE_INFORMATION half_hour{}; half_hour.Bias=-330;
    require(utc_offset_seconds(1704065400, &half_hour)==19800,"offset across year boundary");
    require(utc_offset_seconds(1705320000) % 60 == 0,"system timezone offset");
    pthread_attr_t attr;pthread_t thread;bool stack_ok=false;
    require(pthread_attr_init(&attr)==0 && pthread_attr_setstacksize(&attr,64u<<20)==0,"64MiB thread attributes");
    require(pthread_create(&thread,&attr,thread_main,&stack_ok)==0,"native thread creation");
    pthread_attr_destroy(&attr);require(pthread_join(thread,nullptr)==0 && stack_ok,"large guest-thread host stack");
    std::puts("Windows host I/O, large offsets, concurrency, local time and thread stack passed");
}
