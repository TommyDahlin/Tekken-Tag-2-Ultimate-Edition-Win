// Asset-free driver for the production installer and shortcut implementation.
#include "../runtime/src/install.cpp"
#include "host/digest.h"
extern "C" const cafe::ProgramInfo cafe_program_info = {
    "b97d800ba8d0420dc6a0342045a15a97d785bd61d4cf9902d0c334ba32ec188b", 0,0,0,0,0,0,0};
namespace cafe {
std::string sha256_hex(const std::vector<uint8_t>& data) {
    std::string out; char text[3];
    for (auto byte : host::digest<32>(data)) { std::snprintf(text, sizeof text, "%02x", byte); out += text; }
    return out;
}
}
int main(int argc, char** argv) {
    using namespace cafe::install;
    if (argc == 2 && std::string(argv[1]) == "--test-paths") {
        configure_user_paths();
        std::cout << (std::getenv("TTT2_CHARACTERS") ? std::getenv("TTT2_CHARACTERS") : "") << '\n'
                  << (std::getenv("TTT2_SAVE_DIR") ? std::getenv("TTT2_SAVE_DIR") : "") << '\n';
        return installed_game() ? 0 : 3;
    }
    if (argc == 4 && std::string(argv[1]) == "--test-shortcut") {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        write_shortcut(argv[2], argv[3]); CoUninitialize(); return 0;
    }
    return run(std::vector<std::string>(argv+1, argv+argc));
}
