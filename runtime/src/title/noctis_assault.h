#pragma once
#include <cmath>
#include <string_view>

namespace cafe::title::noctis_assault {
struct Position { float x, z; };
inline bool exit_move(std::string_view code, std::string_view name) {
    return code == "mrz" && name == "Mz_jprp03DA";
}
// The non-attacking aerial Tag Assault exit must remain in the incoming
// partner's half-plane. Reflect only a crossing's longitudinal component;
// retain lateral placement and let the game's physics handle height/facing.
inline bool keep_partner_side(Position& exit, Position partner, Position opponent) {
    const double dx = double(partner.x) - opponent.x;
    const double dz = double(partner.z) - opponent.z;
    const double ex = double(exit.x) - opponent.x;
    const double ez = double(exit.z) - opponent.z;
    const double length2 = dx * dx + dz * dz;
    const double along = ex * dx + ez * dz;
    if (!std::isfinite(length2) || !std::isfinite(along) || length2 < 1.0 || along >= 0)
        return false;
    const double factor = 2 * along / length2;
    exit.x = static_cast<float>(exit.x - factor * dx);
    exit.z = static_cast<float>(exit.z - factor * dz);
    return true;
}
} // namespace cafe::title::noctis_assault
