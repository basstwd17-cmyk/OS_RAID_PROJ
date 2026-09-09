#include "RAID_Device.h"
#include "../ssd/FTL.h"
#include "../ssd/ONFI_Channel_Base.h"
#include <cmath>
#include <iomanip>
#include <stdexcept>

namespace {
double population_sd(const std::vector<double>& values)
{
    if (values.empty()) return 0;
    // Two passes avoid cancellation for high lifetime counts with small spread.
    long double mean = 0, squared = 0;
    for (double v : values) mean += v;
    mean /= values.size();
    for (double v : values) squared += (v - mean) * (v - mean);
    return std::sqrt(static_cast<double>(squared / values.size()));
}
}

void RAID_Device::Configure_telemetry_output(const std::string& prefix)
{
    if (!telemetry_enabled) return;
    telemetry_path = prefix + ".telemetry.csv";
    telemetry.open(telemetry_path, std::ios::out | std::ios::trunc);
    if (!telemetry) throw std::runtime_error("Cannot open telemetry output: " + telemetry_path);
    telemetry << "sample,event,time_ns,migration_sequence,state,mu_write_units,completed_host_requests,completed_host_write_requests,completed_host_write_bytes,queue_requests,queue_request_bytes,copy_buffer_logical_bytes,blocked_requests,migrations,redirects,ssd_id,host_bytes_attributed,completed_nand_page_programs,completed_nand_block_erases,bad_blocks,completed_nand_programmed_bytes\n";
    telemetry << std::setprecision(17);
}

RAID_Device::Snapshot RAID_Device::Capture_snapshot(sim_time_type time, bool detailed) const
{
    Snapshot s;
    s.Time = time;
    s.Host = raid_controller->Get_observation();
    for (unsigned int i = 0; i < ssds.size(); ++i) {
        SSD_Observation o;
        auto ssd = ssds[i];
        auto ftl = static_cast<SSD_Components::FTL*>(ssd->Firmware);
        const auto& cfg = ssd_configs[i].Flash_Parameters;
        o.Bad_blocks = ftl->BlockManager->Get_bad_block_count();
        o.Host_bytes = raid_controller->Get_ssd_attributed_host_write_sectors(i) * SECTOR_SIZE_IN_BYTE;
        std::vector<double> programs, erases;
        for (unsigned int ch = 0; ch < ssd->Channel_count; ++ch) {
            auto channel = static_cast<SSD_Components::ONFI_Channel_Base*>(ssd->Channels[ch]);
            for (unsigned int chip = 0; chip < ssd->Chip_no_per_channel; ++chip) {
                auto flash = channel->Chips[chip];
                o.Programs += flash->Get_total_plane_program_count();
                o.Erases += flash->Get_total_plane_erase_count();
                if (!detailed) continue;
                for (unsigned int die = 0; die < cfg.Die_No_Per_Chip; ++die)
                    for (unsigned int plane = 0; plane < cfg.Plane_No_Per_Die; ++plane) {
                        NVM::FlashMemory::Physical_Page_Address a(ch, chip, die, plane, 0, 0);
                        auto bookkeeping = ftl->BlockManager->Get_plane_bookkeeping_entry(a);
                        for (unsigned int block = 0; block < cfg.Block_No_Per_Plane; ++block) {
                            programs.push_back(static_cast<double>(flash->Get_block_program_count(die, plane, block)));
                            erases.push_back(bookkeeping->Blocks[block].Erase_count);
                        }
                    }
            }
        }
        o.Programmed_bytes = o.Programs * cfg.Page_Capacity;
        o.Block_program_SD = population_sd(programs);
        o.Block_erase_SD = population_sd(erases);
        s.SSDs.push_back(o);
    }
    return s;
}

void RAID_Device::Emit_snapshot(const std::string& event, uint64_t migration, const Snapshot& s)
{
    if (!telemetry.is_open()) return;
    ++telemetry_samples;
    for (unsigned int i = 0; i < s.SSDs.size(); ++i) {
        const auto& o = s.SSDs[i];
        telemetry << telemetry_samples << ',' << event << ',' << s.Time << ',' << migration << ',' << s.Host.State << ',' << s.Host.Mu
            << ',' << s.Host.Completed_requests << ',' << s.Host.Completed_write_requests << ',' << s.Host.Completed_write_bytes
            << ',' << s.Host.Queue_depth << ',' << s.Host.Queue_bytes << ',' << s.Host.Copy_buffer_bytes << ',' << s.Host.Blocked_requests
            << ',' << s.Host.Migrations << ',' << s.Host.Redirects << ',' << i << ',' << o.Host_bytes << ',' << o.Programs << ',' << o.Erases
            << ',' << o.Bad_blocks << ',' << o.Programmed_bytes << '\n';
    }
    telemetry.flush();
}

void RAID_Device::Write_snapshot_XML(const std::string& tag, const Snapshot& s, Utils::XmlWriter& w) const
{
    w.Write_open_tag(tag);
    w.Write_attribute_string("Time_ns", std::to_string(s.Time));
    w.Write_attribute_string("Completed_Host_Requests", std::to_string(s.Host.Completed_requests));
    w.Write_attribute_string("Completed_Host_Write_Requests", std::to_string(s.Host.Completed_write_requests));
    w.Write_attribute_string("Completed_Host_Write_Bytes", std::to_string(s.Host.Completed_write_bytes));
    w.Write_attribute_string("Balance_Unit_Bytes", std::to_string(ssd_configs[0].SWANS_Balance_Unit_Bytes));
    w.Write_attribute_string("Completed_Host_Write_Units", std::to_string(static_cast<double>(s.Host.Completed_write_bytes) / ssd_configs[0].SWANS_Balance_Unit_Bytes));
    w.Write_attribute_string("Mu_Write_Units", std::to_string(s.Host.Mu));
    w.Write_attribute_string("Queue_Requests", std::to_string(s.Host.Queue_depth));
    w.Write_attribute_string("Queue_Request_Bytes", std::to_string(s.Host.Queue_bytes));
    w.Write_attribute_string("Copy_Buffer_Logical_Bytes", std::to_string(s.Host.Copy_buffer_bytes));
    w.Write_attribute_string("Blocked_Requests", std::to_string(s.Host.Blocked_requests));
    std::vector<double> programs, erases, bad;
    for (const auto& o : s.SSDs) { programs.push_back(o.Programs); erases.push_back(o.Erases); bad.push_back(o.Bad_blocks); }
    w.Write_attribute_string("Across_SSD_Program_Count_Population_SD", std::to_string(population_sd(programs)));
    w.Write_attribute_string("Across_SSD_Erase_Count_Population_SD", std::to_string(population_sd(erases)));
    w.Write_attribute_string("Across_SSD_Bad_Block_Count_Population_SD", std::to_string(population_sd(bad)));
    for (unsigned int i = 0; i < s.SSDs.size(); ++i) {
        const auto& o = s.SSDs[i];
        w.Write_open_tag(tag + ".SSD");
        w.Write_attribute_string("SSD_ID", std::to_string(i));
        w.Write_attribute_string("Completed_NAND_Page_Programs", std::to_string(o.Programs));
        w.Write_attribute_string("Completed_NAND_Block_Erases", std::to_string(o.Erases));
        w.Write_attribute_string("Completed_NAND_Programmed_Bytes", std::to_string(o.Programmed_bytes));
        w.Write_attribute_string("Logical_Host_Bytes_Attributed", std::to_string(o.Host_bytes));
        w.Write_attribute_string("Bad_Block_Count", std::to_string(o.Bad_blocks));
        w.Write_attribute_string("All_Blocks_Lifetime_Page_Program_Population_SD", std::to_string(o.Block_program_SD));
        w.Write_attribute_string("All_Blocks_Erase_Count_Population_SD", std::to_string(o.Block_erase_SD));
        w.Write_close_tag();
    }
    w.Write_close_tag();
}
