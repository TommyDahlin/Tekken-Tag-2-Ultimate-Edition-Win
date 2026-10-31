#include "cafe/runtime.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
namespace cafe {
[[noreturn]] void fatal(const char* format, ...) { throw std::runtime_error(format); }
}
void require(bool ok) { if (!ok) std::abort(); }
int main() {
    cafe::reserve_guest_memory();
    auto* base = cafe::guest_base();
    require(base != nullptr);
    cafe::reserve_guest_memory();
    require(base == cafe::guest_base());
    require(!cafe::guest_memory_committed(0x1000, 1));
    cafe::commit_guest_memory(0x1fff, 2);
    require(cafe::guest_memory_committed(0x1000, 0x2000));
    base[0x1fff] = 42; base[0x2000] = 43;
    cafe::commit_guest_memory(0x1000, 0x2000);
    require(base[0x1fff] == 42 && base[0x2000] == 43);
    cafe::commit_guest_memory(0xfffff000, 0x1000);
    base[0xffffffff] = 99;
    require(base[0xffffffff] == 99);
    MEMORY_BASIC_INFORMATION info{};
    require(VirtualQuery(base + (1ull << 32), &info, sizeof(info)) != 0);
    require(info.State == MEM_RESERVE);
    require(!cafe::guest_memory_committed(0xffffffff, 2));
    bool rejected = false;
    try { cafe::commit_guest_memory(0xffffffff, 2); } catch (...) { rejected = true; }
    require(rejected);
    puts("Windows guest memory: reserve, commit, recommit, top boundary and guard passed");
}
