#include "RAID_Controller.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include "../sim/Sim_Defs.h"
#include "../utils/XMLWriter.h"
#include "SSD_Device.h"
#include "../ssd/Stats.h"

static const bool ENABLE_RAID_DEBUG_LOG = false;
static const bool ENABLE_RAID_WARN_LOG = true;
static const bool ENABLE_SWANS_TRACE_LOG = false;
static const int SWANS_EVENT_TYPE = 1;

namespace {
	double to_us(sim_time_type time_in_ns)
	{
		return (double)time_in_ns / (double)SIM_TIME_TO_MICROSECONDS_COEFF;
	}

	double avg_or_zero(uint64_t count, double sum)
	{
		return count == 0 ? 0.0 : sum / (double)count;
	}

	sim_time_type min_or_zero(sim_time_type min_val)
	{
		return min_val == std::numeric_limits<sim_time_type>::max() ? 0 : min_val;
	}

	sim_time_type request_start_time(const SSD_Components::User_Request* request, sim_time_type fallback)
	{
		if (request == nullptr || request->STAT_InitiationTime > fallback) {
			return fallback;
		}
		return request->STAT_InitiationTime;
	}

	std::string swans_state_to_string(RAID_Policy::PolicyState state)
	{
		switch (state) {
			case RAID_Policy::PolicyState::NORMAL:
				return "NORMAL";
			case RAID_Policy::PolicyState::REDIRECT:
				return "REDIRECT";
			case RAID_Policy::PolicyState::MIGRATION:
				return "MIGRATION";
			default:
				return "UNKNOWN";
		}
	}

	void swans_trace(const std::string& message)
	{
		if (!ENABLE_SWANS_TRACE_LOG) {
			return;
		}
		std::cerr << "[SWANS][TRACE] " << message << std::endl;
	}
}

RAID_Controller::RAID_Controller(const sim_object_id_type& id,
	SSD_Components::Host_Interface_Base* host_interface,
	unsigned int stream_count,
	unsigned int ssd_count,
	unsigned int stripe_unit_lba,
	unsigned int zone_block_lba,
	bool swans_enabled,
	unsigned int swans_zone_size_lba,
	unsigned int zone_stripe_multiplier,
	sim_time_type swans_epoch_default,
	sim_time_type swans_epoch_placement,
	sim_time_type swans_epoch_migration,
	double swans_th_precautionary,
	double swans_th_critical,
	unsigned int swans_max_concurrent_migrations,
	unsigned int swans_migration_buffer_limit,
	LHA_type total_logical_lha_count)
	: SSD_Components::Data_Cache_Manager_Base(id, host_interface, nullptr, 1, 1, 1, 0, 0, 0, nullptr,
			SSD_Components::Cache_Sharing_Mode::SHARED, stream_count),
	  ssd_count(ssd_count),
	  stripe_unit_lba(stripe_unit_lba),
	  swans_zone_size_lba(swans_zone_size_lba),
	  swans_enabled(swans_enabled),
	  swans_epoch_default(swans_epoch_default == 0 ? 1 : swans_epoch_default),
	  swans_epoch_placement(swans_epoch_placement == 0 ? (swans_epoch_default == 0 ? 1 : swans_epoch_default) : swans_epoch_placement),
	  swans_epoch_migration(swans_epoch_migration == 0 ? (swans_epoch_default == 0 ? 1 : swans_epoch_default) : swans_epoch_migration),
	  swans_poll_interval((sim_time_type)1),
	  next_policy_evaluation_time(swans_epoch_default == 0 ? 1 : swans_epoch_default),
	  swans_scheduled_event(nullptr),
	  swans_scheduled_event_time(0),
	  swans_policy_state(RAID_Policy::PolicyState::NORMAL),
	  migration_executor(swans_migration_buffer_limit)
{
	per_ssd_stats.resize(ssd_count);
	Set_complete_callback([this](SSD_Components::User_Request* request) {
		if (request->Transaction_list.size() != 0) {
			std::cerr << "[RAID] WARN: Completing request with non-empty Transaction_list! ID=" << request->ID
				<< " Transactions=" << request->Transaction_list.size() << std::endl;
		}
		this->broadcast_user_request_serviced_signal(request);
	});

	if (this->swans_enabled) {
		uint64_t zone_size_lba = this->swans_zone_size_lba;
		if (zone_size_lba == 0) {
			const unsigned int stripes_per_zone = zone_stripe_multiplier == 0 ? 1 : zone_stripe_multiplier;
			zone_size_lba = static_cast<uint64_t>(stripe_unit_lba) * static_cast<uint64_t>(stripes_per_zone);
		}
		try {
			zone_directory.Initialize(ssd_count, stripe_unit_lba, zone_size_lba, zone_block_lba, total_logical_lha_count);
			this->swans_zone_size_lba = zone_directory.Zone_size_lba();
			wear_leveling_policy.Initialize(ssd_count, swans_th_precautionary, swans_th_critical, swans_max_concurrent_migrations);
			swans_poll_interval = std::max((sim_time_type)1, this->swans_epoch_default / (sim_time_type)20);
			next_policy_evaluation_time = this->swans_epoch_default;
			Schedule_swans_event(next_policy_evaluation_time);
		} catch (const std::exception& ex) {
			this->swans_enabled = false;
			std::cerr << "[SWANS] WARN: disabling SWANS due to initialization error: " << ex.what() << std::endl;
		}
	}
}

void RAID_Controller::Set_backend_ssds(const std::vector<SSD_Device*>& ssds)
{
	backend_ssds = ssds;
	Set_submit_callback([this](unsigned int disk_id, SSD_Components::User_Request* sub_request) {
		if (disk_id >= backend_ssds.size() || backend_ssds[disk_id] == nullptr) {
			delete sub_request;
			return;
		}
		backend_ssds[disk_id]->Host_interface->Submit_io_request(sub_request, true);
	});
}

void RAID_Controller::Set_submit_callback(std::function<void(unsigned int, SSD_Components::User_Request*)> callback)
{
	submit_callback = callback;
}

void RAID_Controller::Set_complete_callback(std::function<void(SSD_Components::User_Request*)> callback)
{
	complete_callback = callback;
}

void RAID_Controller::Print_runtime_snapshot(const char* label) const
{
	std::cerr << "[" << label << "] Time=" << Simulator->Time()
		<< " Host_Read=" << raid_request_stats.Completed_read_sectors
		<< " Host_Write=" << raid_request_stats.Completed_write_sectors
		<< " Program=" << SSD_Components::Stats::IssuedProgramCMD
		<< " Erase=" << SSD_Components::Stats::IssuedEraseCMD
		<< " GC=" << SSD_Components::Stats::Total_gc_executions
		<< " WL=" << SSD_Components::Stats::Total_wl_executions
		<< " Redirect=" << swans_stats.Redirect_operations
		<< " Migration=" << swans_stats.Migration_operations
		<< " Migration_BG_Read=" << swans_stats.Background_read_ios
		<< " Migration_BG_Write=" << swans_stats.Background_write_ios << std::endl;
}

void RAID_Controller::Map(LHA_type lba, unsigned int& disk_id, LHA_type& physical_lba) const
{
	if (ssd_count == 0 || stripe_unit_lba == 0) {
		disk_id = 0;
		physical_lba = lba;
		return;
	}
	LHA_type stripe_index = lba / stripe_unit_lba;
	disk_id = static_cast<unsigned int>(stripe_index % ssd_count);
	LHA_type stripe_row = stripe_index / ssd_count;
	LHA_type in_stripe_offset = lba % stripe_unit_lba;
	physical_lba = stripe_row * stripe_unit_lba + in_stripe_offset;
}

bool RAID_Controller::Maybe_apply_redirect(uint64_t zone_id)
{
	if (!swans_enabled || swans_policy_state != RAID_Policy::PolicyState::REDIRECT || !swans_last_decision.Redirect.Valid) {
		swans_trace("redirect skip: policy inactive");
		return false;
	}
	if (zone_directory.Is_migrating(zone_id) || !zone_directory.Is_empty(zone_id)) {
		std::ostringstream oss;
		oss << "redirect skip zone=" << zone_id
			<< " reason=not_empty_or_migrating"
			<< " empty=" << (zone_directory.Is_empty(zone_id) ? 1 : 0)
			<< " migrating=" << (zone_directory.Is_migrating(zone_id) ? 1 : 0);
		swans_trace(oss.str());
		return false;
	}
	const unsigned int owner = zone_directory.Owner_ssd(zone_id);
	if (owner != swans_last_decision.Redirect.Hot_ssd) {
		std::ostringstream oss;
		oss << "redirect skip zone=" << zone_id
			<< " reason=owner_not_hot"
			<< " owner_ssd=" << owner
			<< " hot_ssd=" << swans_last_decision.Redirect.Hot_ssd
			<< " cold_ssd=" << swans_last_decision.Redirect.Cold_ssd;
		swans_trace(oss.str());
		return false;
	}

	std::vector<uint64_t> reserved;
	uint64_t cold_zone = zone_directory.Find_empty_zone_on_ssd(swans_last_decision.Redirect.Cold_ssd, reserved);
	if (cold_zone == RAID_Policy::INVALID_ZONE_ID || cold_zone == zone_id || zone_directory.Is_migrating(cold_zone)) {
		std::ostringstream oss;
		oss << "redirect skip zone=" << zone_id
			<< " reason=no_valid_cold_empty_zone"
			<< " cold_zone=" << cold_zone;
		swans_trace(oss.str());
		return false;
	}

	zone_directory.Swap_placement(zone_id, cold_zone);
	swans_stats.Redirect_operations++;
	{
		std::ostringstream oss;
		oss << "redirect apply logical_zone=" << zone_id
			<< " from_hot_ssd=" << swans_last_decision.Redirect.Hot_ssd
			<< " to_cold_ssd=" << swans_last_decision.Redirect.Cold_ssd
			<< " swapped_with_zone=" << cold_zone
			<< " reason=empty_zone&&owner_hot";
		swans_trace(oss.str());
	}
	return true;
}

std::vector<RAID_Sub_Request> RAID_Controller::Split(LHA_type lba, unsigned int lba_count, SSD_Components::UserRequestType type, stream_id_type stream_id)
{
	std::vector<RAID_Sub_Request> parts;
	if (lba_count == 0 || ssd_count == 0) {
		if (lba_count > 0) {
			parts.push_back({ 0, lba, lba_count, 0, 0 });
		}
		return parts;
	}

	if (swans_enabled && zone_directory.Is_initialized()) {
		LHA_type current_lba = lba;
		unsigned int remaining = lba_count;
		while (remaining > 0) {
			RAID_Policy::ZoneResolveResult resolved;
			zone_directory.Resolve(current_lba, resolved);
			const unsigned int chunk = Swans_mapping_chunk_length(resolved, remaining);

			uint64_t zone_id = resolved.Zone_id;
			if (type == SSD_Components::UserRequestType::WRITE && swans_policy_state == RAID_Policy::PolicyState::REDIRECT) {
				Maybe_apply_redirect(zone_id);
				zone_directory.Resolve(current_lba, resolved);
				zone_id = resolved.Zone_id;
			}

			if (type == SSD_Components::UserRequestType::WRITE) {
				zone_directory.Observe_write(stream_id, zone_id, resolved.Zone_lba_offset, chunk);
				wear_leveling_policy.Observe_host_write(resolved.Disk_id, (uint64_t)chunk * SECTOR_SIZE_IN_BYTE);
			}

			parts.push_back({ resolved.Disk_id, resolved.Local_lba, chunk, zone_id, resolved.Stripe_offset });
			current_lba += chunk;
			remaining -= chunk;
		}
		return parts;
	}

	if (stripe_unit_lba == 0) {
		parts.push_back({ 0, lba, lba_count, 0, 0 });
		return parts;
	}

	LHA_type current_lba = lba;
	unsigned int remaining = lba_count;
	while (remaining > 0) {
		unsigned int base_disk = 0;
		LHA_type base_lba = 0;
		Map(current_lba, base_disk, base_lba);
		unsigned int in_stripe_offset = static_cast<unsigned int>(current_lba % stripe_unit_lba);
		unsigned int stripe_remaining = stripe_unit_lba - in_stripe_offset;
		unsigned int chunk = remaining < stripe_remaining ? remaining : stripe_remaining;

		parts.push_back({ base_disk, base_lba, chunk, 0, 0 });
		current_lba += chunk;
		remaining -= chunk;
	}
	return parts;
}

SSD_Components::User_Request* RAID_Controller::Create_sub_request(const SSD_Components::User_Request* original, const RAID_Sub_Request& part) const
{
	SSD_Components::User_Request* sub_request = new SSD_Components::User_Request;
	sub_request->Priority_class = original->Priority_class;
	sub_request->Start_LBA = part.Start_LBA;
	sub_request->SizeInSectors = part.LBA_count;
	sub_request->Size_in_byte = part.LBA_count * SECTOR_SIZE_IN_BYTE;
	sub_request->Type = original->Type;
	sub_request->Stream_id = original->Stream_id;
	sub_request->ToBeIgnored = original->ToBeIgnored;
	sub_request->STAT_InitiationTime = original->STAT_InitiationTime;
	sub_request->IO_command_info = nullptr;
	sub_request->Data = nullptr;
	return sub_request;
}

io_request_id_type RAID_Controller::Submit_background_copy(const RAID_Policy::StripeCopyPlan& copy, bool is_write, uint64_t task_index)
{
	if (!submit_callback) {
		return io_request_id_type();
	}

	SSD_Components::User_Request* req = new SSD_Components::User_Request;
	req->Priority_class = IO_Flow_Priority_Class::Priority::LOW;
	req->Start_LBA = is_write ? copy.Destination_lba : copy.Source_lba;
	req->SizeInSectors = copy.Lba_count;
	req->Size_in_byte = copy.Lba_count * SECTOR_SIZE_IN_BYTE;
	req->Type = is_write ? SSD_Components::UserRequestType::WRITE : SSD_Components::UserRequestType::READ;
	req->Stream_id = copy.Stream_id;
	req->ToBeIgnored = true;
	req->STAT_InitiationTime = Simulator->Time();
	req->IO_command_info = nullptr;
	req->Data = nullptr;

	Sub_Request_Metadata metadata;
	metadata.Parent = nullptr;
	metadata.Disk_id = is_write ? copy.Destination_disk_id : copy.Source_disk_id;
	metadata.Size_in_sectors = copy.Lba_count;
	metadata.Type = req->Type;
	metadata.Submit_time = Simulator->Time();
	metadata.Is_background = true;
	metadata.Migration_task_index = task_index;
	metadata.Migration_is_write = is_write;
	subrequest_metadata_by_id[req->ID] = metadata;

	if (is_write) {
		swans_stats.Background_write_ios++;
		wear_leveling_policy.Observe_migration_write(metadata.Disk_id, copy.Lba_count);
	} else {
		swans_stats.Background_read_ios++;
	}

	submit_callback(metadata.Disk_id, req);
	return req->ID;
}

bool RAID_Controller::Discard_migration_source(const RAID_Policy::StripeCopyPlan& copy, uint64_t)
{
	if (copy.Lba_count == 0 || copy.Source_disk_id >= backend_ssds.size() || backend_ssds[copy.Source_disk_id] == nullptr) {
		return false;
	}
	backend_ssds[copy.Source_disk_id]->Discard_logical_range(copy.Stream_id, copy.Source_lba, copy.Lba_count);
	swans_stats.Source_discard_requests++;
	swans_stats.Source_discard_sectors += copy.Lba_count;
	return true;
}

void RAID_Controller::Observe_buffered_hot_write(const RAID_Policy::MigrationTask& task, const SSD_Components::User_Request* request)
{
	if (request == nullptr || request->Type != SSD_Components::UserRequestType::WRITE || request->SizeInSectors == 0
		|| !zone_directory.Is_initialized()) {
		return;
	}

	LHA_type current_lba = request->Start_LBA;
	unsigned int remaining = request->SizeInSectors;
	while (remaining > 0) {
		RAID_Policy::ZoneResolveResult resolved;
		zone_directory.Resolve(current_lba, resolved);
		const unsigned int chunk = Swans_mapping_chunk_length(resolved, remaining);
		if (resolved.Zone_id == task.Op.Hot_zone && chunk > 0) {
			zone_directory.Observe_write(request->Stream_id, resolved.Zone_id, resolved.Zone_lba_offset, chunk);
			wear_leveling_policy.Observe_host_write(task.Op.Cold_ssd, (uint64_t)chunk * SECTOR_SIZE_IN_BYTE);
			if (task.Op.Cold_ssd < per_ssd_stats.size()) {
				per_ssd_stats[task.Op.Cold_ssd].Attributed_host_write_sectors += chunk;
			}
		}
		current_lba += chunk;
		remaining -= chunk;
	}
}

void RAID_Controller::Submit(SSD_Components::User_Request* request)
{
	sim_time_type now = Simulator->Time();
	const sim_time_type request_start = request_start_time(request, now);
	std::vector<RAID_Sub_Request> parts = Split(request->Start_LBA, request->SizeInSectors, request->Type, request->Stream_id);

	raid_request_stats.Submitted_requests++;
	if (request->Type == SSD_Components::UserRequestType::READ) {
		raid_request_stats.Submitted_read_requests++;
		raid_request_stats.Submitted_read_sectors += request->SizeInSectors;
	} else {
		raid_request_stats.Submitted_write_requests++;
		raid_request_stats.Submitted_write_sectors += request->SizeInSectors;
	}
	raid_request_stats.Total_subrequests_dispatched += parts.size();

	Inflight_Entry entry;
	entry.Pending = (unsigned int)parts.size();
	entry.Total_sub_requests = (unsigned int)parts.size();
	entry.Submit_time = request_start;
	inflight[request->ID] = entry;

	if (!submit_callback) {
		return;
	}
	if (parts.empty()) {
		const sim_time_type completion_latency = now >= request_start ? now - request_start : 0;
		inflight.erase(request->ID);
		raid_request_stats.Completed_requests++;
		raid_request_stats.Sum_request_completion_latency += completion_latency;
		raid_request_stats.Min_request_completion_latency = std::min(raid_request_stats.Min_request_completion_latency, completion_latency);
		if (completion_latency > raid_request_stats.Max_request_completion_latency) {
			raid_request_stats.Max_request_completion_latency = completion_latency;
		}
		if (complete_callback) {
			complete_callback(request);
		}
		return;
	}

	for (size_t i = 0; i < parts.size(); i++) {
		RAID_Sub_Request& part = parts[i];
		SSD_Components::User_Request* sub_request = Create_sub_request(request, part);
		Sub_Request_Metadata metadata;
		metadata.Parent = request;
		metadata.Disk_id = part.Disk_id;
		metadata.Size_in_sectors = part.LBA_count;
		metadata.Zone_id = part.Zone_id;
		metadata.Type = sub_request->Type;
		metadata.Submit_time = now;
		subrequest_metadata_by_id[sub_request->ID] = metadata;

		if (part.Disk_id < per_ssd_stats.size()) {
			Per_SSD_Stats& stats = per_ssd_stats[part.Disk_id];
			stats.Submitted_subrequests++;
			stats.Submitted_sectors += part.LBA_count;
			if (sub_request->Type == SSD_Components::UserRequestType::READ) {
				stats.Submitted_read_subrequests++;
				stats.Submitted_read_sectors += part.LBA_count;
			} else {
				stats.Submitted_write_subrequests++;
				stats.Submitted_write_sectors += part.LBA_count;
				stats.Attributed_host_write_sectors += part.LBA_count;
			}
		}
		submit_callback(part.Disk_id, sub_request);
	}
}

void RAID_Controller::Complete_buffered_user_request(SSD_Components::User_Request* request)
{
	if (request == nullptr) {
		return;
	}

	sim_time_type now = Simulator->Time();
	const sim_time_type request_start = request_start_time(request, now);
	raid_request_stats.Submitted_requests++;
	if (request->Type == SSD_Components::UserRequestType::READ) {
		raid_request_stats.Submitted_read_requests++;
		raid_request_stats.Submitted_read_sectors += request->SizeInSectors;
		raid_request_stats.Completed_read_sectors += request->SizeInSectors;
	} else {
		raid_request_stats.Submitted_write_requests++;
		raid_request_stats.Submitted_write_sectors += request->SizeInSectors;
		raid_request_stats.Completed_write_sectors += request->SizeInSectors;
	}
	raid_request_stats.Completed_requests++;

	const sim_time_type latency = now >= request_start ? now - request_start : 0;
	raid_request_stats.Sum_request_completion_latency += latency;
	if (latency < raid_request_stats.Min_request_completion_latency) {
		raid_request_stats.Min_request_completion_latency = latency;
	}
	if (latency > raid_request_stats.Max_request_completion_latency) {
		raid_request_stats.Max_request_completion_latency = latency;
	}

	if (complete_callback) {
		complete_callback(request);
	}
}

unsigned int RAID_Controller::Swans_mapping_chunk_length(const RAID_Policy::ZoneResolveResult& resolved, unsigned int remaining) const
{
	if (remaining == 0) {
		return 0;
	}
	const uint64_t stripe_size = zone_directory.Stripe_unit_lba();
	const uint64_t zone_size = zone_directory.Zone_size_lba();
	const uint64_t stripe_remaining = stripe_size > resolved.In_stripe_offset
		? stripe_size - resolved.In_stripe_offset : 1;
	const uint64_t zone_remaining = zone_size > resolved.Zone_lba_offset
		? zone_size - resolved.Zone_lba_offset : 1;
	const uint64_t chunk = std::min<uint64_t>(remaining, std::min<uint64_t>(stripe_remaining, zone_remaining));
	return static_cast<unsigned int>(chunk == 0 ? 1 : chunk);
}

std::vector<uint64_t> RAID_Controller::Collect_zone_ids(LHA_type lba, unsigned int lba_count) const
{
	std::vector<uint64_t> zone_ids;
	if (!swans_enabled || !zone_directory.Is_initialized() || lba_count == 0) {
		return zone_ids;
	}

	LHA_type current_lba = lba;
	unsigned int remaining = lba_count;
	while (remaining > 0) {
		RAID_Policy::ZoneResolveResult resolved;
		zone_directory.Resolve(current_lba, resolved);
		if (std::find(zone_ids.begin(), zone_ids.end(), resolved.Zone_id) == zone_ids.end()) {
			zone_ids.push_back(resolved.Zone_id);
		}
		const unsigned int chunk = Swans_mapping_chunk_length(resolved, remaining);
		current_lba += chunk;
		remaining -= chunk;
	}
	return zone_ids;
}

std::vector<RAID_Policy::MigrationTask> RAID_Controller::Build_migration_tasks(const std::vector<RAID_Policy::MigrationOp>& ops) const
{
	std::vector<RAID_Policy::MigrationTask> tasks;
	const uint64_t zone_size_lba = zone_directory.Zone_size_lba();
	const unsigned int block_unit_lba = zone_directory.Block_unit_lba();
	for (size_t i = 0; i < ops.size(); i++) {
		const RAID_Policy::MigrationOp& op = ops[i];
		std::map<stream_id_type, std::vector<unsigned int>> blocks_by_stream =
			zone_directory.Written_block_offsets_by_stream(op.Hot_zone);
		if (blocks_by_stream.empty()) {
			continue;
		}
		RAID_Policy::MigrationTask task;
		task.Op = op;
		task.Moved_write_count = zone_directory.Zone_write_count(op.Hot_zone);
		for (std::map<stream_id_type, std::vector<unsigned int>>::const_iterator stream_it = blocks_by_stream.begin();
			stream_it != blocks_by_stream.end(); ++stream_it) {
			task.Copies.reserve(task.Copies.size() + stream_it->second.size());
			for (size_t j = 0; j < stream_it->second.size(); j++) {
				const uint64_t zone_lba_offset = static_cast<uint64_t>(stream_it->second[j]) * block_unit_lba;
				if (zone_lba_offset >= zone_size_lba) {
					continue;
				}
				const uint64_t remaining_zone_lba = zone_size_lba - zone_lba_offset;
				const uint64_t copy_lba_count_64 = std::min<uint64_t>(block_unit_lba, remaining_zone_lba);
				if (copy_lba_count_64 == 0 || copy_lba_count_64 > std::numeric_limits<unsigned int>::max()) {
					continue;
				}

				RAID_Policy::StripeCopyPlan copy;
				copy.Stream_id = stream_it->first;
				copy.Stripe_offset = stream_it->second[j];
				copy.Lba_count = static_cast<unsigned int>(copy_lba_count_64);
				zone_directory.Resolve_zone_lba(op.Hot_zone, zone_lba_offset, copy.Source_disk_id, copy.Source_lba);
				zone_directory.Resolve_zone_lba(op.Cold_zone, zone_lba_offset, copy.Destination_disk_id, copy.Destination_lba);
				task.Copies.push_back(copy);
			}
		}
		if (!task.Copies.empty()) {
			tasks.push_back(task);
		}
	}
	return tasks;
}

bool RAID_Controller::Has_inflight_user_io_on_migrating_zone() const
{
	for (const auto& item : subrequest_metadata_by_id) {
		const Sub_Request_Metadata& metadata = item.second;
		if (!metadata.Is_background
			&& metadata.Zone_id != std::numeric_limits<uint64_t>::max()
			&& zone_directory.Is_migrating(metadata.Zone_id)) {
			return true;
		}
	}
	return false;
}

void RAID_Controller::Notify_sub_request_completed(SSD_Components::User_Request* sub_request)
{
	sim_time_type now = Simulator->Time();
	auto metadata_it = subrequest_metadata_by_id.find(sub_request->ID);
	if (metadata_it == subrequest_metadata_by_id.end()) {
		if (ENABLE_RAID_WARN_LOG) {
			std::cerr << "[RAID] WARN: completion for unknown sub_request ID=" << sub_request->ID << std::endl;
		}
		return;
	}
	Sub_Request_Metadata metadata = metadata_it->second;
	subrequest_metadata_by_id.erase(metadata_it);

	if (metadata.Is_background) {
		if (migration_executor.Notify_request_completed(sub_request->ID, zone_directory)) {
			Schedule_swans_event(now + 1);
		}
		return;
	}

	SSD_Components::User_Request* original_request = metadata.Parent;
	sim_time_type sub_latency = now >= metadata.Submit_time ? now - metadata.Submit_time : 0;
	if (metadata.Disk_id < per_ssd_stats.size()) {
		Per_SSD_Stats& stats = per_ssd_stats[metadata.Disk_id];
		stats.Completed_subrequests++;
		stats.Completed_sectors += metadata.Size_in_sectors;
		if (metadata.Type == SSD_Components::UserRequestType::READ) {
			stats.Completed_read_sectors += metadata.Size_in_sectors;
		} else {
			stats.Completed_write_sectors += metadata.Size_in_sectors;
		}
		stats.Sum_subrequest_latency += sub_latency;
		if (sub_latency < stats.Min_subrequest_latency) {
			stats.Min_subrequest_latency = sub_latency;
		}
		if (sub_latency > stats.Max_subrequest_latency) {
			stats.Max_subrequest_latency = sub_latency;
		}
	}

	auto it = inflight.find(original_request->ID);
	if (it == inflight.end()) {
		if (ENABLE_RAID_WARN_LOG) {
			std::cerr << "[RAID] WARN: inflight entry missing for parent ID=" << original_request->ID << std::endl;
		}
		return;
	}

	if (it->second.Pending > 0) {
		it->second.Pending--;
	}
	if (sub_latency < it->second.Min_subrequest_latency) {
		it->second.Min_subrequest_latency = sub_latency;
	}
	if (sub_latency > it->second.Max_subrequest_latency) {
		it->second.Max_subrequest_latency = sub_latency;
	}

	if (it->second.Pending == 0) {
		sim_time_type request_completion_latency = now >= it->second.Submit_time ? now - it->second.Submit_time : 0;
		sim_time_type min_sub = min_or_zero(it->second.Min_subrequest_latency);
		sim_time_type completion_skew = it->second.Total_sub_requests > 0 && it->second.Max_subrequest_latency >= min_sub
			? it->second.Max_subrequest_latency - min_sub : 0;

		raid_request_stats.Completed_requests++;
		if (original_request->Type == SSD_Components::UserRequestType::READ) {
			raid_request_stats.Completed_read_sectors += original_request->SizeInSectors;
		} else {
			raid_request_stats.Completed_write_sectors += original_request->SizeInSectors;
		}
		raid_request_stats.Sum_request_completion_latency += request_completion_latency;
		if (request_completion_latency < raid_request_stats.Min_request_completion_latency) {
			raid_request_stats.Min_request_completion_latency = request_completion_latency;
		}
		if (request_completion_latency > raid_request_stats.Max_request_completion_latency) {
			raid_request_stats.Max_request_completion_latency = request_completion_latency;
		}
		raid_request_stats.Sum_completion_skew += completion_skew;
		if (completion_skew > raid_request_stats.Max_completion_skew) {
			raid_request_stats.Max_completion_skew = completion_skew;
		}

		inflight.erase(it);
		if (complete_callback) {
			complete_callback(original_request);
		}
	}
}

void RAID_Controller::Notify_sub_transaction_completed(unsigned int disk_id, SSD_Components::NVM_Transaction* transaction)
{
	if (disk_id < per_ssd_stats.size() && transaction != nullptr) {
		Per_SSD_Stats& stats = per_ssd_stats[disk_id];
		sim_time_type now = Simulator->Time();
		sim_time_type turnaround = now >= transaction->Issue_time ? now - transaction->Issue_time : 0;
		sim_time_type execution = transaction->STAT_execution_time == INVALID_TIME ? 0 : transaction->STAT_execution_time;
		sim_time_type transfer = transaction->STAT_transfer_time == INVALID_TIME ? 0 : transaction->STAT_transfer_time;
		sim_time_type waiting = turnaround >= execution + transfer ? turnaround - execution - transfer : 0;

		if (transaction->Type == SSD_Components::Transaction_Type::READ) {
			stats.Completed_read_transactions++;
			stats.Sum_read_transaction_turnaround += turnaround;
			stats.Sum_read_transaction_execution += execution;
			stats.Sum_read_transaction_transfer += transfer;
			stats.Sum_read_transaction_waiting += waiting;
		} else if (transaction->Type == SSD_Components::Transaction_Type::WRITE) {
			stats.Completed_write_transactions++;
			stats.Sum_write_transaction_turnaround += turnaround;
			stats.Sum_write_transaction_execution += execution;
			stats.Sum_write_transaction_transfer += transfer;
			stats.Sum_write_transaction_waiting += waiting;
		}
	}
	broadcast_user_memory_transaction_serviced_signal(transaction);
}

void RAID_Controller::Try_replay_blocked_requests()
{
	if (blocked_user_requests.empty()) {
		return;
	}

	const size_t initial = blocked_user_requests.size();
	for (size_t i = 0; i < initial; i++) {
		SSD_Components::User_Request* request = blocked_user_requests.front();
		blocked_user_requests.pop_front();
		if (request == nullptr) {
			continue;
		}

		if (swans_enabled && migration_executor.Has_inflight()) {
			std::vector<uint64_t> zones = Collect_zone_ids(request->Start_LBA, request->SizeInSectors);
			RAID_Policy::MigrationExecutor::InterceptResult intercepted =
				migration_executor.Maybe_intercept(request, zones, zone_directory,
					[this](const RAID_Policy::MigrationTask& task, const SSD_Components::User_Request* buffered_request) {
						this->Observe_buffered_hot_write(task, buffered_request);
					});
			if (intercepted == RAID_Policy::MigrationExecutor::InterceptResult::SUBMITTED) {
				Submit(request);
				continue;
			}
			if (intercepted == RAID_Policy::MigrationExecutor::InterceptResult::BUFFERED) {
				swans_stats.Buffered_requests++;
				if (request->Type == SSD_Components::UserRequestType::WRITE) {
					swans_stats.Buffered_write_requests++;
					swans_stats.Buffered_write_sectors += request->SizeInSectors;
				}
				continue;
			}

			// BACKPRESSURE persists: keep order and stop retrying in this round.
			blocked_user_requests.push_front(request);
			break;
		}

		Submit(request);
	}
}

void RAID_Controller::Schedule_swans_event(sim_time_type fire_time)
{
	if (!swans_enabled) {
		return;
	}
	sim_time_type now = Simulator->Time();
	if (fire_time <= now) {
		fire_time = now + 1;
	}
	if (swans_scheduled_event != nullptr) {
		if (swans_scheduled_event_time <= fire_time) {
			return;
		}
		Simulator->Ignore_sim_event(swans_scheduled_event);
	}
	swans_scheduled_event = Simulator->Register_sim_event(fire_time, this, nullptr, SWANS_EVENT_TYPE);
	swans_scheduled_event_time = fire_time;
}

void RAID_Controller::Handle_swans_event()
{
	if (!swans_enabled) {
		return;
	}

	sim_time_type now = Simulator->Time();
	if (migration_executor.Has_inflight()) {
		// Start marks both zones first. New requests are buffered, while requests
		// dispatched before the mark drain before the first source copy begins.
		if (Has_inflight_user_io_on_migrating_zone()) {
			swans_stats.Migration_barrier_waits++;
			Schedule_swans_event(now + swans_poll_interval);
			return;
		}
		migration_executor.Poll(zone_directory,
			[this](const RAID_Policy::StripeCopyPlan& copy, bool is_write, uint64_t task_index) -> io_request_id_type {
				return this->Submit_background_copy(copy, is_write, task_index);
			},
			[this](const RAID_Policy::StripeCopyPlan& copy, uint64_t task_index) -> bool {
				return this->Discard_migration_source(copy, task_index);
			},
			[this](const RAID_Policy::MigrationTask& task, uint64_t moved_write_count) {
				this->wear_leveling_policy.Transfer_writes(task.Op.Hot_ssd, task.Op.Cold_ssd, moved_write_count);
			});

		std::vector<RAID_Policy::MigrationExecutor::DeferredRequest> replay = migration_executor.Drain_replay_requests();
		for (size_t i = 0; i < replay.size(); i++) {
			swans_stats.Replay_requests++;
			const sim_time_type wait_time = now >= replay[i].Enqueue_time ? now - replay[i].Enqueue_time : 0;
			swans_stats.Migration_total_waiting_time += wait_time;
			if (wait_time > swans_stats.Migration_max_waiting_time) {
				swans_stats.Migration_max_waiting_time = wait_time;
			}
			if (replay[i].Request != nullptr && replay[i].Request->Type == SSD_Components::UserRequestType::READ) {
				swans_stats.Migration_waiting_read_requests++;
			} else {
				swans_stats.Migration_waiting_write_requests++;
			}
			if (replay[i].Complete_without_dispatch) {
				swans_stats.Buffered_write_completions++;
				Complete_buffered_user_request(replay[i].Request);
			} else {
				Submit(replay[i].Request);
			}
		}

		if (migration_executor.Has_inflight()) {
			Schedule_swans_event(now + swans_poll_interval);
			return;
		}
	}

	Try_replay_blocked_requests();

	if (now >= next_policy_evaluation_time) {
		swans_last_decision = wear_leveling_policy.Evaluate(zone_directory);
		swans_stats.Epoch_evaluations++;
		swans_stats.Last_mu = swans_last_decision.Mu;

		if (swans_policy_state != swans_last_decision.State) {
			swans_stats.State_transitions++;
		}
		swans_policy_state = swans_last_decision.State;
		{
			std::ostringstream oss;
			oss << "epoch_eval t=" << now
				<< " mu=" << swans_last_decision.Mu
				<< " state=" << swans_state_to_string(swans_policy_state);
			if (swans_policy_state == RAID_Policy::PolicyState::REDIRECT && swans_last_decision.Redirect.Valid) {
				oss << " hot_ssd=" << swans_last_decision.Redirect.Hot_ssd
					<< " cold_ssd=" << swans_last_decision.Redirect.Cold_ssd;
			}
			if (swans_policy_state == RAID_Policy::PolicyState::MIGRATION) {
				oss << " migration_candidates=" << swans_last_decision.Migrations.size();
				if (!swans_last_decision.Migrations.empty()) {
					oss << " first_hot_zone=" << swans_last_decision.Migrations[0].Hot_zone
						<< " first_cold_zone=" << swans_last_decision.Migrations[0].Cold_zone
						<< " hot_ssd=" << swans_last_decision.Migrations[0].Hot_ssd
						<< " cold_ssd=" << swans_last_decision.Migrations[0].Cold_ssd;
				}
			}
			swans_trace(oss.str());
		}

		switch (swans_policy_state) {
			case RAID_Policy::PolicyState::NORMAL:
				swans_stats.Normal_epochs++;
				break;
			case RAID_Policy::PolicyState::REDIRECT:
				swans_stats.Redirect_epochs++;
				break;
			case RAID_Policy::PolicyState::MIGRATION:
				swans_stats.Migration_epochs++;
				break;
		}

		if (swans_policy_state == RAID_Policy::PolicyState::MIGRATION && !swans_last_decision.Migrations.empty()) {
			std::vector<RAID_Policy::MigrationTask> tasks = Build_migration_tasks(swans_last_decision.Migrations);
			if (!tasks.empty()) {
				swans_stats.Migration_operations += tasks.size();
				for (const RAID_Policy::MigrationTask& task : tasks) {
					Swans_Migration_Record record;
					record.Sequence = swans_migration_history.size();
					record.Start_time = now;
					record.Hot_zone = task.Op.Hot_zone;
					record.Cold_zone = task.Op.Cold_zone;
					record.Hot_ssd = task.Op.Hot_ssd;
					record.Cold_ssd = task.Op.Cold_ssd;
					record.Copy_blocks = task.Copies.size();
					std::vector<stream_id_type> seen_streams;
					for (const RAID_Policy::StripeCopyPlan& copy : task.Copies) {
						record.Copy_sectors += copy.Lba_count;
						if (std::find(seen_streams.begin(), seen_streams.end(), copy.Stream_id) == seen_streams.end()) {
							seen_streams.push_back(copy.Stream_id);
						}
					}
					record.Stream_count = seen_streams.size();
					if (!task.Copies.empty()) {
						const RAID_Policy::StripeCopyPlan& first_copy = task.Copies.front();
						record.First_stream_id = first_copy.Stream_id;
						record.First_source_disk = first_copy.Source_disk_id;
						record.First_destination_disk = first_copy.Destination_disk_id;
						record.First_source_lba = first_copy.Source_lba;
						record.First_destination_lba = first_copy.Destination_lba;
					}
					swans_migration_history.push_back(record);
				}
				migration_executor.Start(tasks, zone_directory);
				Schedule_swans_event(now + 1);
				{
					std::ostringstream oss;
					oss << "migration start task_count=" << tasks.size();
					swans_trace(oss.str());
				}
			}
		}

		sim_time_type next_epoch_interval = swans_epoch_default;
		switch (swans_policy_state) {
			case RAID_Policy::PolicyState::NORMAL:
				next_epoch_interval = swans_epoch_default;
				break;
			case RAID_Policy::PolicyState::REDIRECT:
				next_epoch_interval = swans_epoch_placement;
				break;
			case RAID_Policy::PolicyState::MIGRATION:
				next_epoch_interval = swans_epoch_migration;
				break;
		}
		next_policy_evaluation_time = now + std::max((sim_time_type)1, next_epoch_interval);
	}

	const bool has_inflight_user_or_background = !inflight.empty() || !subrequest_metadata_by_id.empty();
	const bool has_epoch_writes = wear_leveling_policy.Has_epoch_writes();
	if (migration_executor.Has_inflight()) {
		Schedule_swans_event(now + swans_poll_interval);
	} else if (has_inflight_user_or_background || has_epoch_writes) {
		Schedule_swans_event(next_policy_evaluation_time);
	}
}

void RAID_Controller::Do_warmup(std::vector<Utils::Workload_Statistics*>)
{
}

void RAID_Controller::Execute_simulator_event(MQSimEngine::Sim_Event* event)
{
	if (event == nullptr || event->Type != SWANS_EVENT_TYPE || event != swans_scheduled_event) {
		return;
	}
	swans_scheduled_event = nullptr;
	swans_scheduled_event_time = 0;
	Handle_swans_event();
}

void RAID_Controller::process_new_user_request(SSD_Components::User_Request* user_request)
{
	if (swans_enabled && user_request->Type == SSD_Components::UserRequestType::WRITE && swans_scheduled_event == nullptr) {
		Schedule_swans_event(Simulator->Time() + swans_poll_interval);
	}

	if (swans_enabled && migration_executor.Has_inflight()) {
		std::vector<uint64_t> zones = Collect_zone_ids(user_request->Start_LBA, user_request->SizeInSectors);
		RAID_Policy::MigrationExecutor::InterceptResult intercepted =
			migration_executor.Maybe_intercept(user_request, zones, zone_directory,
				[this](const RAID_Policy::MigrationTask& task, const SSD_Components::User_Request* buffered_request) {
					this->Observe_buffered_hot_write(task, buffered_request);
				});
		if (intercepted == RAID_Policy::MigrationExecutor::InterceptResult::BUFFERED) {
			swans_stats.Buffered_requests++;
			if (user_request->Type == SSD_Components::UserRequestType::WRITE) {
				swans_stats.Buffered_write_requests++;
				swans_stats.Buffered_write_sectors += user_request->SizeInSectors;
			}
			if (swans_scheduled_event == nullptr) {
				Schedule_swans_event(Simulator->Time() + swans_poll_interval);
			}
			return;
		}
		if (intercepted == RAID_Policy::MigrationExecutor::InterceptResult::BACKPRESSURE) {
			blocked_user_requests.push_back(user_request);
			swans_stats.Buffered_requests++;
			if (user_request->Type == SSD_Components::UserRequestType::WRITE) {
				swans_stats.Buffered_write_requests++;
				swans_stats.Buffered_write_sectors += user_request->SizeInSectors;
			}
			if (swans_scheduled_event == nullptr) {
				Schedule_swans_event(Simulator->Time() + swans_poll_interval);
			}
			return;
		}
	}
	Submit(user_request);
}

void RAID_Controller::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter) const
{
	std::string tag = name_prefix + ".RAIDController";
	xmlwriter.Write_open_tag(tag);

	xmlwriter.Write_attribute_string("SSD_Count", std::to_string(ssd_count));
	xmlwriter.Write_attribute_string("Stripe_Unit_LBA", std::to_string(stripe_unit_lba));
	xmlwriter.Write_attribute_string("SWANS_Zone_Size_LBA", std::to_string(swans_zone_size_lba));
	xmlwriter.Write_attribute_string("Submitted_Requests", std::to_string(raid_request_stats.Submitted_requests));
	xmlwriter.Write_attribute_string("Completed_Requests", std::to_string(raid_request_stats.Completed_requests));
	xmlwriter.Write_attribute_string("Submitted_Read_Requests", std::to_string(raid_request_stats.Submitted_read_requests));
	xmlwriter.Write_attribute_string("Submitted_Write_Requests", std::to_string(raid_request_stats.Submitted_write_requests));
	xmlwriter.Write_attribute_string("Inflight_Requests", std::to_string(inflight.size()));
	xmlwriter.Write_attribute_string("Inflight_SubRequests", std::to_string(subrequest_metadata_by_id.size()));
	xmlwriter.Write_attribute_string("Total_SubRequests_Dispatched", std::to_string(raid_request_stats.Total_subrequests_dispatched));
	xmlwriter.Write_attribute_string("Submitted_Read_Sectors", std::to_string(raid_request_stats.Submitted_read_sectors));
	xmlwriter.Write_attribute_string("Submitted_Write_Sectors", std::to_string(raid_request_stats.Submitted_write_sectors));
	xmlwriter.Write_attribute_string("Completed_Read_Sectors", std::to_string(raid_request_stats.Completed_read_sectors));
	xmlwriter.Write_attribute_string("Completed_Write_Sectors", std::to_string(raid_request_stats.Completed_write_sectors));

	double avg_split = raid_request_stats.Submitted_requests == 0 ? 0.0
		: (double)raid_request_stats.Total_subrequests_dispatched / (double)raid_request_stats.Submitted_requests;
	xmlwriter.Write_attribute_string("Average_Split_Count_Per_Request", std::to_string(avg_split));
	double write_request_split_amp = raid_request_stats.Submitted_write_requests == 0 ? 0.0
		: (double)Get_total_subrequest_write_sectors() / (double)raid_request_stats.Submitted_write_sectors;
	double write_subrequest_per_request = raid_request_stats.Submitted_write_requests == 0 ? 0.0 :
		(double)std::accumulate(per_ssd_stats.begin(), per_ssd_stats.end(), (uint64_t)0,
			[](uint64_t acc, const Per_SSD_Stats& s) { return acc + s.Submitted_write_subrequests; })
		/ (double)raid_request_stats.Submitted_write_requests;
	xmlwriter.Write_attribute_string("Logical_Write_Sector_Amplification", std::to_string(write_request_split_amp));
	xmlwriter.Write_attribute_string("Average_Write_SubRequests_Per_Write_Request", std::to_string(write_subrequest_per_request));

	xmlwriter.Write_attribute_string("Average_Request_Completion_Latency_us",
		std::to_string(avg_or_zero(raid_request_stats.Completed_requests, to_us(raid_request_stats.Sum_request_completion_latency))));
	xmlwriter.Write_attribute_string("Min_Request_Completion_Latency_us",
		std::to_string(to_us(min_or_zero(raid_request_stats.Min_request_completion_latency))));
	xmlwriter.Write_attribute_string("Max_Request_Completion_Latency_us",
		std::to_string(to_us(raid_request_stats.Max_request_completion_latency)));
	xmlwriter.Write_attribute_string("Average_SubRequest_Completion_Skew_us",
		std::to_string(avg_or_zero(raid_request_stats.Completed_requests, to_us(raid_request_stats.Sum_completion_skew))));
	xmlwriter.Write_attribute_string("Max_SubRequest_Completion_Skew_us",
		std::to_string(to_us(raid_request_stats.Max_completion_skew)));

	for (unsigned int disk_id = 0; disk_id < per_ssd_stats.size(); disk_id++) {
		const Per_SSD_Stats& stats = per_ssd_stats[disk_id];
		std::string disk_tag = tag + ".SSD";
		xmlwriter.Write_open_tag(disk_tag);

		xmlwriter.Write_attribute_string("SSD_ID", std::to_string(disk_id));
		xmlwriter.Write_attribute_string("Submitted_SubRequests", std::to_string(stats.Submitted_subrequests));
		xmlwriter.Write_attribute_string("Completed_SubRequests", std::to_string(stats.Completed_subrequests));
		xmlwriter.Write_attribute_string("Submitted_Read_SubRequests", std::to_string(stats.Submitted_read_subrequests));
		xmlwriter.Write_attribute_string("Submitted_Write_SubRequests", std::to_string(stats.Submitted_write_subrequests));
		xmlwriter.Write_attribute_string("Submitted_Sectors", std::to_string(stats.Submitted_sectors));
		xmlwriter.Write_attribute_string("Completed_Sectors", std::to_string(stats.Completed_sectors));
		xmlwriter.Write_attribute_string("Submitted_Read_Sectors", std::to_string(stats.Submitted_read_sectors));
		xmlwriter.Write_attribute_string("Submitted_Write_Sectors", std::to_string(stats.Submitted_write_sectors));
		xmlwriter.Write_attribute_string("Attributed_Host_Write_Sectors", std::to_string(stats.Attributed_host_write_sectors));
		xmlwriter.Write_attribute_string("Completed_Read_Sectors", std::to_string(stats.Completed_read_sectors));
		xmlwriter.Write_attribute_string("Completed_Write_Sectors", std::to_string(stats.Completed_write_sectors));
		xmlwriter.Write_attribute_string("Completed_Read_Transactions", std::to_string(stats.Completed_read_transactions));
		xmlwriter.Write_attribute_string("Completed_Write_Transactions", std::to_string(stats.Completed_write_transactions));
		xmlwriter.Write_attribute_string("Average_Read_Transaction_Turnaround_us", std::to_string(avg_or_zero(stats.Completed_read_transactions, to_us(stats.Sum_read_transaction_turnaround))));
		xmlwriter.Write_attribute_string("Average_Read_Transaction_Execution_us", std::to_string(avg_or_zero(stats.Completed_read_transactions, to_us(stats.Sum_read_transaction_execution))));
		xmlwriter.Write_attribute_string("Average_Read_Transaction_Transfer_us", std::to_string(avg_or_zero(stats.Completed_read_transactions, to_us(stats.Sum_read_transaction_transfer))));
		xmlwriter.Write_attribute_string("Average_Read_Transaction_Waiting_us", std::to_string(avg_or_zero(stats.Completed_read_transactions, to_us(stats.Sum_read_transaction_waiting))));
		xmlwriter.Write_attribute_string("Average_Write_Transaction_Turnaround_us", std::to_string(avg_or_zero(stats.Completed_write_transactions, to_us(stats.Sum_write_transaction_turnaround))));
		xmlwriter.Write_attribute_string("Average_Write_Transaction_Execution_us", std::to_string(avg_or_zero(stats.Completed_write_transactions, to_us(stats.Sum_write_transaction_execution))));
		xmlwriter.Write_attribute_string("Average_Write_Transaction_Transfer_us", std::to_string(avg_or_zero(stats.Completed_write_transactions, to_us(stats.Sum_write_transaction_transfer))));
		xmlwriter.Write_attribute_string("Average_Write_Transaction_Waiting_us", std::to_string(avg_or_zero(stats.Completed_write_transactions, to_us(stats.Sum_write_transaction_waiting))));
		xmlwriter.Write_attribute_string("Average_SubRequest_Latency_us",
			std::to_string(avg_or_zero(stats.Completed_subrequests, to_us(stats.Sum_subrequest_latency))));
		xmlwriter.Write_attribute_string("Min_SubRequest_Latency_us",
			std::to_string(to_us(min_or_zero(stats.Min_subrequest_latency))));
		xmlwriter.Write_attribute_string("Max_SubRequest_Latency_us",
			std::to_string(to_us(stats.Max_subrequest_latency)));

		xmlwriter.Write_close_tag();
	}

	xmlwriter.Write_attribute_string("SWANS_Enabled", swans_enabled ? "true" : "false");
	xmlwriter.Write_attribute_string("SWANS_State", swans_state_to_string(swans_policy_state));
	xmlwriter.Write_attribute_string("SWANS_Last_Mu", std::to_string(swans_stats.Last_mu));
	xmlwriter.Write_attribute_string("SWANS_Epoch_Evaluations", std::to_string(swans_stats.Epoch_evaluations));
	xmlwriter.Write_attribute_string("SWANS_Normal_Epochs", std::to_string(swans_stats.Normal_epochs));
	xmlwriter.Write_attribute_string("SWANS_Redirect_Epochs", std::to_string(swans_stats.Redirect_epochs));
	xmlwriter.Write_attribute_string("SWANS_Migration_Epochs", std::to_string(swans_stats.Migration_epochs));
	xmlwriter.Write_attribute_string("SWANS_State_Transitions", std::to_string(swans_stats.State_transitions));
	xmlwriter.Write_attribute_string("SWANS_Redirect_Operations", std::to_string(swans_stats.Redirect_operations));
	xmlwriter.Write_attribute_string("SWANS_Migration_Operations", std::to_string(swans_stats.Migration_operations));
	xmlwriter.Write_attribute_string("SWANS_Migration_Barrier_Waits", std::to_string(swans_stats.Migration_barrier_waits));
	xmlwriter.Write_attribute_string("SWANS_Migration_History_Count", std::to_string(swans_migration_history.size()));
	xmlwriter.Write_attribute_string("SWANS_Buffered_Requests", std::to_string(swans_stats.Buffered_requests));
	xmlwriter.Write_attribute_string("SWANS_DMH_Buffered_Write_Requests", std::to_string(swans_stats.Buffered_write_requests));
	xmlwriter.Write_attribute_string("SWANS_DMH_Buffered_Write_Sectors", std::to_string(swans_stats.Buffered_write_sectors));
	xmlwriter.Write_attribute_string("SWANS_Migration_Waiting_Read_Requests", std::to_string(swans_stats.Migration_waiting_read_requests));
	xmlwriter.Write_attribute_string("SWANS_Migration_Waiting_Write_Requests", std::to_string(swans_stats.Migration_waiting_write_requests));
	xmlwriter.Write_attribute_string("SWANS_Migration_Total_Waiting_Time_us", std::to_string(to_us(swans_stats.Migration_total_waiting_time)));
	xmlwriter.Write_attribute_string("SWANS_Migration_Average_Waiting_Time_us",
		std::to_string((swans_stats.Migration_waiting_read_requests + swans_stats.Migration_waiting_write_requests) == 0 ? 0.0 :
			to_us(swans_stats.Migration_total_waiting_time) / (double)(swans_stats.Migration_waiting_read_requests + swans_stats.Migration_waiting_write_requests)));
	xmlwriter.Write_attribute_string("SWANS_Migration_Max_Waiting_Time_us", std::to_string(to_us(swans_stats.Migration_max_waiting_time)));
	xmlwriter.Write_attribute_string("SWANS_Replay_Requests", std::to_string(swans_stats.Replay_requests));
	xmlwriter.Write_attribute_string("SWANS_Buffered_Write_Completions", std::to_string(swans_stats.Buffered_write_completions));
	xmlwriter.Write_attribute_string("SWANS_Background_Read_IOs", std::to_string(swans_stats.Background_read_ios));
	xmlwriter.Write_attribute_string("SWANS_Background_Write_IOs", std::to_string(swans_stats.Background_write_ios));
	xmlwriter.Write_attribute_string("SWANS_Source_Discard_Requests", std::to_string(swans_stats.Source_discard_requests));
	xmlwriter.Write_attribute_string("SWANS_Source_Discard_Sectors", std::to_string(swans_stats.Source_discard_sectors));
	xmlwriter.Write_attribute_string("SWANS_Grabbed_Blocks", std::to_string(migration_executor.Grabbed_blocks()));
	xmlwriter.Write_attribute_string("SWANS_Restored_Blocks", std::to_string(migration_executor.Restored_blocks()));
	xmlwriter.Write_attribute_string("SWANS_Discarded_Source_Blocks", std::to_string(migration_executor.Discarded_source_blocks()));
	xmlwriter.Write_attribute_string("SWANS_Discarded_Source_Sectors", std::to_string(migration_executor.Discarded_source_sectors()));
	xmlwriter.Write_attribute_string("SWANS_Dirty_Buffer_Blocks", std::to_string(migration_executor.Dirty_blocks()));
	xmlwriter.Write_attribute_string("SWANS_Working_Queue_Max_Depth", std::to_string(migration_executor.Max_queue_depth()));
	xmlwriter.Write_attribute_string("SWANS_Buffered_Inflight", std::to_string(migration_executor.Buffered_count()));
	xmlwriter.Write_attribute_string("SWANS_Backpressure_Events", std::to_string(migration_executor.Backpressure_events()));
	xmlwriter.Write_attribute_string("SWANS_Blocked_Requests", std::to_string(blocked_user_requests.size()));
	xmlwriter.Write_attribute_string("SWANS_Migration_Active", migration_executor.Has_inflight() ? "true" : "false");
	xmlwriter.Write_attribute_string("SWANS_Migrating_Zones", std::to_string(zone_directory.Migrating_zone_count()));
	xmlwriter.Write_attribute_string("SWANS_Mapping_Entries", std::to_string(zone_directory.Zone_count()));
	xmlwriter.Write_attribute_string("SWANS_Mapping_Duplicate_Physical_Locations",
		std::to_string(zone_directory.Duplicate_physical_location_count()));

	for (size_t i = 0; i < swans_migration_history.size(); i++) {
		const Swans_Migration_Record& record = swans_migration_history[i];
		const std::string record_tag = "SWANS_Migration_Record_" + std::to_string(i);
		xmlwriter.Write_open_tag(record_tag);
		xmlwriter.Write_attribute_string("Sequence", std::to_string(record.Sequence));
		xmlwriter.Write_attribute_string("Start_Time", std::to_string(record.Start_time));
		xmlwriter.Write_attribute_string("Hot_SSD", std::to_string(record.Hot_ssd));
		xmlwriter.Write_attribute_string("Cold_SSD", std::to_string(record.Cold_ssd));
		xmlwriter.Write_attribute_string("Hot_Zone", std::to_string(record.Hot_zone));
		xmlwriter.Write_attribute_string("Cold_Zone", std::to_string(record.Cold_zone));
		xmlwriter.Write_attribute_string("First_Stream_ID", std::to_string(record.First_stream_id));
		xmlwriter.Write_attribute_string("Stream_Count", std::to_string(record.Stream_count));
		xmlwriter.Write_attribute_string("First_Source_Disk", std::to_string(record.First_source_disk));
		xmlwriter.Write_attribute_string("First_Destination_Disk", std::to_string(record.First_destination_disk));
		xmlwriter.Write_attribute_string("First_Source_LBA", std::to_string(record.First_source_lba));
		xmlwriter.Write_attribute_string("First_Destination_LBA", std::to_string(record.First_destination_lba));
		xmlwriter.Write_attribute_string("Copy_Blocks", std::to_string(record.Copy_blocks));
		xmlwriter.Write_attribute_string("Copy_Sectors", std::to_string(record.Copy_sectors));
		xmlwriter.Write_close_tag();
	}

	xmlwriter.Write_close_tag();
}

uint64_t RAID_Controller::Get_total_host_write_sectors() const
{
	return raid_request_stats.Submitted_write_sectors;
}

uint64_t RAID_Controller::Get_total_subrequest_write_sectors() const
{
	uint64_t total = 0;
	for (size_t i = 0; i < per_ssd_stats.size(); i++) {
		total += per_ssd_stats[i].Submitted_write_sectors;
	}
	return total;
}

uint64_t RAID_Controller::Get_ssd_subrequest_write_sectors(unsigned int disk_id) const
{
	if (disk_id >= per_ssd_stats.size()) {
		return 0;
	}
	return per_ssd_stats[disk_id].Submitted_write_sectors;
}

uint64_t RAID_Controller::Get_total_attributed_host_write_sectors() const
{
	uint64_t total = 0;
	for (size_t i = 0; i < per_ssd_stats.size(); i++) {
		total += per_ssd_stats[i].Attributed_host_write_sectors;
	}
	return total;
}

uint64_t RAID_Controller::Get_ssd_attributed_host_write_sectors(unsigned int disk_id) const
{
	if (disk_id >= per_ssd_stats.size()) {
		return 0;
	}
	return per_ssd_stats[disk_id].Attributed_host_write_sectors;
}
