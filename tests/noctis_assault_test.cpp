#include "title/noctis_assault.h"
#include "title/noctis_hitbox.h"
#include <cassert>
#include <cmath>
#include <limits>
using namespace cafe::title::noctis_assault;
int main() {
    assert(exit_move("mrz","Mz_jprp03DA"));
    assert(!exit_move("mrz","Mz_jprp03") && !exit_move("mrz","Mz_jprp03T"));
    assert(!exit_move("mrz","Mz_jpWPFDA") && !exit_move("elz","Mz_jprp03DA"));
    Position p{-700,120};
    assert(keep_partner_side(p,{1500,0},{0,0}));
    assert(p.x==700 && p.z==120);
    assert(!keep_partner_side(p,{1500,0},{0,0})); // stable after correction
    p={700,120};
    assert(keep_partner_side(p,{-1500,0},{0,0})); // opposite screen side
    assert(p.x==-700 && p.z==120);
    p={25,-900};
    assert(keep_partner_side(p,{0,1200},{0,0})); // rotated stage/camera
    assert(p.x==25 && p.z==900);
    p={-300,-500};
    const float lateral=p.x-p.z;
    assert(keep_partner_side(p,{1000,1000},{0,0}));
    assert(p.x+p.z>0 && std::abs((p.x-p.z)-lateral)<0.001f);
    p={-400,700};
    assert(!keep_partner_side(p,{0,0},{0,0})); // no defined partner side
    assert(p.x==-400 && p.z==700);
    assert(!keep_partner_side(p,{std::numeric_limits<float>::infinity(),0},{0,0}));
    p={-600,600};
    assert(keep_partner_side(p,{1500,500},{500,500})); // translated stage origin
    assert(p.x==1600 && p.z==600);
    using cafe::title::noctis_hitbox::lower_wall_point;
    using cafe::title::noctis_hitbox::below_tip;
    assert(lower_wall_point("mrz","Mz_t_srk01F"));
    assert(lower_wall_point("mrz","Mz_t_srk01F_C"));
    assert(lower_wall_point("mrz","Mz_t_srk02Fy"));
    assert(!lower_wall_point("mrz","Mz_t_srk00F")); // first hit
    assert(!lower_wall_point("mrz","Mz_t_srk02F")); // third hit
    assert(!lower_wall_point("mrz","Mz_rush01"));
    assert(!lower_wall_point("elz","Mz_t_srk01F"));
    const auto low=below_tip({1200,1400,-200});
    assert(low.x==1200 && low.y==1100 && low.z==-200);

}
