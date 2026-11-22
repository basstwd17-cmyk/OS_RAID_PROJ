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
    if (ssd_configs[0].SWANS_Enabled) {
        migration_events.open(prefix + ".migrations.csv", std::ios::out | std::ios::trunc);
        if (!migration_events) throw std::runtime_error("Cannot open migration event output");
        migration_events << "sample,event,time_ns,time_s,migration_sequence,status,execution_mode,forced_drain,force_reason,force_time_s,start_time_s,end_time_s,elapsed_s,duration_s,previous_migration_sequence,seconds_since_previous_start,seconds_since_previous_end,previous_migration_duration_s,source_ssd,destination_ssd,source_zone,destination_zone,reserve_zone,space_mode,reverse_source_ssd,reverse_destination_ssd,reverse_source_zone,reverse_destination_zone,ssd_id,host_write_bytes_attributed,host_write_units,completed_nand_programmed_bytes,completed_nand_page_programs,completed_nand_block_erases,block_erase_min,block_erase_max,block_erase_mean,block_erase_sd,erase_limit,mean_erase_wear_percent,max_erase_wear_percent,tec_valid,tec,tec_wear_percent,bad_blocks,bad_block_wear_percent,remaining_op_ratio\n";
        migration_events << std::setprecision(17);
    }
    if (!telemetry_enabled) return;
    telemetry_path = prefix + ".telemetry.csv";
    telemetry.open(telemetry_path, std::ios::out | std::ios::trunc);
    if (!telemetry) throw std::runtime_error("Cannot open telemetry output: " + telemetry_path);
    telemetry << "sample,event,time_ns,migration_sequence,state,mu,completed_host_requests,completed_host_write_requests,completed_host_write_bytes,queue_requests,queue_request_bytes,copy_buffer_logical_bytes,blocked_requests,migrations,redirects,ssd_id,host_bytes_attributed,completed_nand_page_programs,completed_nand_block_erases,bad_blocks,completed_nand_programmed_bytes,tec_valid,tec,erase_limit,wear_percent,user_block_boundary,remaining_op_ratio,mu_unit\n";
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
        o.Wear = ftl->BlockManager->Get_wear_snapshot();
        o.Remaining_op_ratio = ftl->BlockManager->Get_current_op_ratio();
        o.Bad_block_wear_percent = 100.0 * RAID_Policy::Bad_block_wear(o.Bad_blocks,
            ftl->BlockManager->Get_total_block_count(), ftl->BlockManager->Get_initial_op_block_budget(),
            ftl->BlockManager->Get_end_of_life_threshold());
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
        if (!erases.empty()) {
            const auto bounds = std::minmax_element(erases.begin(), erases.end());
            o.Min_erase_count = static_cast<unsigned int>(*bounds.first);
            o.Max_erase_count = static_cast<unsigned int>(*bounds.second);
            long double sum = 0;
            for (double count : erases) sum += count;
            o.Mean_erase_count = static_cast<double>(sum / erases.size());
        }
        s.SSDs.push_back(o);
    }
    return s;
}

void RAID_Device::Emit_migration_event(const std::string& event, uint64_t migration, const Snapshot& s)
{
    const auto& record = raid_controller->Get_migration_record(migration);
    const auto* previous = migration ? &raid_controller->Get_migration_record(migration - 1) : nullptr;
    const bool completed = record.Status == "COMPLETED";
    const bool ended = record.Status != "ACTIVE";
    const bool previous_ended = previous && previous->Status != "ACTIVE";
    const bool previous_completed = previous && previous->Status == "COMPLETED";
    const double elapsed = static_cast<double>(s.Time - record.Start_time) / ONE_SECOND;
    ++migration_event_samples;
    for (unsigned int i = 0; i < s.SSDs.size(); ++i) {
        const auto& o = s.SSDs[i];
        migration_events << migration_event_samples << ',' << event << ',' << s.Time
            << ',' << static_cast<double>(s.Time) / ONE_SECOND << ',' << migration << ',' << record.Status
            << ',' << (raid_controller->Lazy_migration_enabled() ? "ON_DEMAND" : "EAGER")
            << ',' << (record.Force_reason != "NONE") << ',' << record.Force_reason << ',';
        if (record.Force_reason != "NONE") migration_events << static_cast<double>(record.Force_time) / ONE_SECOND;
        migration_events << ',' << static_cast<double>(record.Start_time) / ONE_SECOND << ',';
        if (ended) migration_events << static_cast<double>(record.End_time) / ONE_SECOND;
        migration_events << ',' << elapsed << ',';
        if (completed) migration_events << static_cast<double>(record.End_time - record.Start_time) / ONE_SECOND;
        migration_events << ',';
        if (previous) migration_events << previous->Sequence;
        migration_events << ',';
        if (previous) migration_events << static_cast<double>(s.Time - previous->Start_time) / ONE_SECOND;
        migration_events << ',';
        if (previous_ended) migration_events << static_cast<double>(s.Time - previous->End_time) / ONE_SECOND;
        migration_events << ',';
        if (previous_completed) migration_events << static_cast<double>(previous->End_time - previous->Start_time) / ONE_SECOND;
        migration_events << ',' << record.Hot_ssd << ',' << record.Cold_ssd
            << ',' << record.Hot_zone << ',' << record.Cold_zone << ',';
        if (record.Reserve_zone != RAID_Policy::INVALID_ZONE_ID) migration_events << record.Reserve_zone;
        migration_events << ',' << (record.Reserve_zone == RAID_Policy::INVALID_ZONE_ID ? "EMPTY_DESTINATION" : "RESERVE_EXCHANGE");
        if (record.Reserve_zone != RAID_Policy::INVALID_ZONE_ID) {
            migration_events << ',' << record.Cold_ssd << ',' << record.Hot_ssd
                << ',' << record.Cold_zone << ',' << record.Reserve_zone;
        } else {
            migration_events << ",,,,";
        }
        migration_events << ',' << i << ',' << o.Host_bytes
            << ',' << static_cast<double>(o.Host_bytes) / ssd_configs[0].SWANS_Balance_Unit_Bytes
            << ',' << o.Programmed_bytes << ',' << o.Programs << ',' << o.Erases
            << ',' << o.Min_erase_count << ',' << o.Max_erase_count << ',' << o.Mean_erase_count
            << ',' << o.Block_erase_SD << ',' << o.Wear.Erase_limit << ',';
        if (o.Wear.Erase_limit) migration_events << 100.0 * o.Mean_erase_count / o.Wear.Erase_limit;
        migration_events << ',';
        if (o.Wear.Erase_limit) migration_events << 100.0 * o.Max_erase_count / o.Wear.Erase_limit;
        migration_events << ',' << o.Wear.Valid << ',';
        if (o.Wear.Valid) migration_events << o.Wear.Tail_erase_count;
        migration_events << ',';
        if (o.Wear.Valid) migration_events << o.Wear.Wear_percent;
        migration_events << ',' << o.Bad_blocks << ',';
        if (std::isfinite(o.Bad_block_wear_percent)) migration_events << o.Bad_block_wear_percent;
        migration_events << ',' << o.Remaining_op_ratio << '\n';
    }
    migration_events.flush();
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
            << ',' << o.Bad_blocks << ',' << o.Programmed_bytes
            << ',' << o.Wear.Valid << ',' << o.Wear.Tail_erase_count << ',' << o.Wear.Erase_limit
            << ',' << o.Wear.Wear_percent << ',' << o.Wear.User_blocks << ',' << o.Remaining_op_ratio
            << ',' << (ssd_configs[0].SWANS_Wear_Metric == "TEC" ? "percent_points" : "write_units") << '\n';
    }
    telemetry.flush();
}

void RAID_Device::Write_snapshot_XML(const std::string& tag, const Snapshot& s, Utils::XmlWriter& w) const
{
    w.Write_open_tag(tag);
    w.Write_attribute_string("Time_ns", std::to_string(s.Time));
    w.Write_attribute_string("End_of_Life_Mode", ssd_configs[0].End_of_Life_Mode);
    w.Write_attribute_string("End_of_Life_Threshold_Unit", ssd_configs[0].End_of_Life_Mode == "TEC" ? "percent" : "remaining_op_ratio");
    w.Write_attribute_string("End_of_Life_Active_Threshold", std::to_string(ssd_configs[0].End_of_Life_Mode == "TEC"
        ? ssd_configs[0].End_of_Life_TEC_Threshold_Percent : ssd_configs[0].End_of_Life_Threshold));
    w.Write_attribute_string("Completed_Host_Requests", std::to_string(s.Host.Completed_requests));
    w.Write_attribute_string("Completed_Host_Write_Requests", std::to_string(s.Host.Completed_write_requests));
    w.Write_attribute_string("Completed_Host_Write_Bytes", std::to_string(s.Host.Completed_write_bytes));
    w.Write_attribute_string("Balance_Unit_Bytes", std::to_string(ssd_configs[0].SWANS_Balance_Unit_Bytes));
    w.Write_attribute_string("Completed_Host_Write_Units", std::to_string(static_cast<double>(s.Host.Completed_write_bytes) / ssd_configs[0].SWANS_Balance_Unit_Bytes));
    w.Write_attribute_string("Mu", std::to_string(s.Host.Mu));
    w.Write_attribute_string("Mu_Unit", ssd_configs[0].SWANS_Wear_Metric == "TEC" ? "percent_points" : "write_units");
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
        w.Write_attribute_string("TEC_Valid", o.Wear.Valid ? "true" : "false");
        w.Write_attribute_string("TEC_Error", o.Wear.Error);
        w.Write_attribute_string("TEC", std::to_string(o.Wear.Tail_erase_count));
        w.Write_attribute_string("Erase_Limit", std::to_string(o.Wear.Erase_limit));
        w.Write_attribute_string("Wear_Percent", std::to_string(o.Wear.Wear_percent));
        w.Write_attribute_string("TEC_User_Block_Boundary", std::to_string(o.Wear.User_blocks));
        w.Write_attribute_string("Logical_Capacity_In_Blocks", std::to_string(logical_blocks_per_ssd));
        w.Write_attribute_string("TEC_Boundary_Minus_Logical_Blocks", std::to_string(o.Wear.User_blocks - logical_blocks_per_ssd));
        w.Write_attribute_string("Remaining_OP_Ratio", std::to_string(o.Remaining_op_ratio));
        w.Write_attribute_string("All_Blocks_Lifetime_Page_Program_Population_SD", std::to_string(o.Block_program_SD));
        w.Write_attribute_string("All_Blocks_Erase_Count_Population_SD", std::to_string(o.Block_erase_SD));
        w.Write_close_tag();
    }
    w.Write_close_tag();
}
