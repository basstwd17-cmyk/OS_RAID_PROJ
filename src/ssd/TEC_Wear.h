#ifndef MQSIM_TEC_WEAR_H
#define MQSIM_TEC_WEAR_H
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace SSD_Components {
struct TECWearSnapshot {
    bool Valid = false;
    std::string Error;
    uint64_t Total_blocks = 0, User_blocks = 0;
    unsigned int Tail_erase_count = 0, Erase_limit = 0;
    double Wear_percent = std::numeric_limits<double>::quiet_NaN();
};

// Work on a copy: block locations and retired blocks must remain intact.
inline TECWearSnapshot Calculate_TEC(std::vector<unsigned int> counts,
    uint64_t initial_op_blocks, unsigned int erase_limit)
{
    TECWearSnapshot s;
    s.Total_blocks = counts.size();
    s.Erase_limit = erase_limit;
    if (!erase_limit || !initial_op_blocks || initial_op_blocks >= counts.size()) {
        s.Error = "TEC requires positive endurance and 0 < initial OP blocks < total blocks";
        return s;
    }
    s.User_blocks = counts.size() - initial_op_blocks;
    std::nth_element(counts.begin(), counts.begin() + s.User_blocks, counts.end());
    s.Tail_erase_count = counts[s.User_blocks];
    s.Wear_percent = 100.0 * s.Tail_erase_count / erase_limit;
    s.Valid = true;
    return s;
}
}
#endif
