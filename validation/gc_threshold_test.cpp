#include "../src/ssd/Flash_Block_Manager.h"
#include "../src/ssd/GC_and_WL_Unit_Page_Level.h"
#include <algorithm>
#include <cassert>
#include <iostream>

using namespace SSD_Components;
using NVM::FlashMemory::Physical_Page_Address;

int main()
{
    Flash_Block_Manager manager("threshold-test", nullptr, 2, 1,
        2, 2, 2, 2, 16, 8, .5, true, 0);
    GC_and_WL_Unit_Page_Level gc("gc-test", nullptr, &manager, nullptr, nullptr,
        GC_Block_Selection_Policy_Type::GREEDY, .25, true, .125,
        2, 2, 2, 2, 16, 8, 8, false, 1, 2, false, false, 0, 1);
    manager.Set_GC_and_WL_Unit(&gc);
    for (unsigned int channel = 0; channel < 2; ++channel) {
        for (unsigned int chip = 0; chip < 2; ++chip) {
            for (unsigned int die = 0; die < 2; ++die) {
                for (unsigned int plane = 0; plane < 2; ++plane) {
                    Physical_Page_Address address(channel, chip, die, plane);
                    assert(gc.Get_gc_threshold_for_plane(address) == 4);
                    assert(gc.Get_gc_hard_threshold_for_plane(address) == 2);
                }
            }
        }
    }
    Physical_Page_Address target(1, 1, 1, 1);
    auto* plane = manager.Get_plane_bookkeeping_entry(target);
    auto* block = plane->Get_a_free_block(0, false);
    target.BlockID = block->BlockID;
    manager.Add_erased_block_to_pool(target);
    assert(!block->Is_bad && manager.Get_bad_block_count() == 0);
    assert(gc.Get_gc_threshold_for_plane(target) == 4);
    assert(gc.Get_gc_hard_threshold_for_plane(target) == 2);

    // Retire the same block after its second erase through the production path.
    auto entry = std::find_if(plane->Free_block_pool.begin(), plane->Free_block_pool.end(),
        [block](const std::pair<const unsigned int, Block_Pool_Slot_Type*>& item) {
            return item.second == block;
        });
    assert(entry != plane->Free_block_pool.end());
    plane->Free_block_pool.erase(entry);
    manager.Add_erased_block_to_pool(target);
    assert(block->Is_bad && manager.Get_bad_block_count() == 1);
    assert(gc.Get_gc_threshold_for_plane(target) == 3);
    assert(gc.Get_gc_hard_threshold_for_plane(target) == 1);

    // Further retirements reach the existing minimum thresholds.
    for (unsigned int retired = 2; retired <= 12; ++retired) {
        block = plane->Get_a_free_block(0, false);
        target.BlockID = block->BlockID;
        block->Erase_count = 1;
        manager.Add_erased_block_to_pool(target);
        assert(manager.Get_bad_block_count() == retired);
        assert(gc.Get_gc_threshold_for_plane(target) == std::max(2U, (16 - retired) / 4));
        assert(gc.Get_gc_hard_threshold_for_plane(target) == std::max(1U, (16 - retired) / 8));
        assert(gc.Get_gc_threshold_for_plane(Physical_Page_Address()) == 4);
        assert(gc.Get_gc_hard_threshold_for_plane(Physical_Page_Address()) == 2);
    }
    std::cout << "GC threshold initialization, normal erase, retirement and minimum tests passed\n";
}
