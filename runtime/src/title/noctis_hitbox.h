#pragma once
#include <string_view>
namespace cafe::title::noctis_hitbox {
inline bool lower_wall_point(std::string_view code, std::string_view move) {
    return code == "mrz" && (move == "Mz_t_srk01F" || move == "Mz_t_srk01F_C" || move == "Mz_t_srk02Fy");
}
struct Point { float x, y, z; };
inline Point below_tip(Point tip) { return {tip.x, tip.y - 300.0f, tip.z}; }
} // namespace cafe::title::noctis_hitbox
