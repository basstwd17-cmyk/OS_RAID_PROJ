#ifndef RAID_CONTROLLER_H
#define RAID_CONTROLLER_H

#include <functional>
#include <deque>
#include <limits>
#include <unordered_map>
#include <vector>
#include "../policy/migration_executor.h"
#include "../policy/wear_leveling_policy.h"
#include "../policy/zone_directory.h"
#include "../ssd/Data_Cache_Manager_Base.h"
#include "../ssd/SSD_Defs.h"
#include "../ssd/User_Request.h"

class SSD_Device;
namespace Utils { class XmlWriter; }

struct RAID_Sub_Request
{
	unsigned int Disk_id;
	LHA_type Start_LBA;
	unsigned int LBA_count;
	uint64_t Zone_id;
	unsigned int Stripe_offset;
};

class RAID_Controller : public SSD_Components::Data_Cache_Manager_Base
{
public:
	RAID_Controller(const sim_object_id_type& id,
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
		LHA_type total_logical_lha_count);

	void Set_backend_ssds(const std::vector<SSD_Device*>& ssds);
	void Set_submit_callback(std::function<void(unsigned int, SSD_Components::User_Request*)> callback);
	void Set_complete_callback(std::function<void(SSD_Components::User_Request*)> callback);

	void Submit(SSD_Components::User_Request* request);
	void Notify_sub_request_completed(SSD_Components::User_Request* sub_request);
	void Notify_sub_transaction_completed(unsigned int disk_id, SSD_Components::NVM_Transaction* transaction);
	void Map(LHA_type lba, unsigned int& disk_id, LHA_type& physical_lba) const;
	std::vector<RAID_Sub_Request> Split(LHA_type lba, unsigned int lba_count, SSD_Components::UserRequestType type, stream_id_type stream_id);
	void Do_warmup(std::vector<Utils::Workload_Statistics*> workload_stats) override;
	void Execute_simulator_event(MQSimEngine::Sim_Event* event) override;
	void Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter) const;
	uint64_t Get_total_host_write_sectors() const;
	uint64_t Get_total_subrequest_write_sectors() const;
	uint64_t Get_ssd_subrequest_write_sectors(unsigned int disk_id) const;
	uint64_t Get_total_attributed_host_write_sectors() const;
	uint64_t Get_ssd_attributed_host_write_sectors(unsigned int disk_id) const;

private:
	struct Inflight_Entry
	{
		unsigned int Pending = 0;
		unsigned int Total_sub_requests = 0;
		sim_time_type Submit_time = 0;
		sim_time_type Min_subrequest_latency = std::numeric_limits<sim_time_type>::max();
		sim_time_type Max_subrequest_latency = 0;
	};

	struct Sub_Request_Metadata
	{
		SSD_Components::User_Request* Parent = nullptr;
		unsigned int Disk_id = 0;
		unsigned int Size_in_sectors = 0;
		uint64_t Zone_id = std::numeric_limits<uint64_t>::max();
		SSD_Components::UserRequestType Type = SSD_Components::UserRequestType::READ;
		sim_time_type Submit_time = 0;
		bool Is_background = false;
		uint64_t Migration_task_index = 0;
		bool Migration_is_write = false;
	};

	struct Per_SSD_Stats
	{
		uint64_t Submitted_subrequests = 0;
		uint64_t Completed_subrequests = 0;
		uint64_t Submitted_read_subrequests = 0;
		uint64_t Submitted_write_subrequests = 0;
		uint64_t Submitted_sectors = 0;
		uint64_t Completed_sectors = 0;
		uint64_t Submitted_read_sectors = 0;
		uint64_t Submitted_write_sectors = 0;
		uint64_t Attributed_host_write_sectors = 0;
		uint64_t Completed_read_sectors = 0;
		uint64_t Completed_write_sectors = 0;
		uint64_t Completed_read_transactions = 0;
		uint64_t Completed_write_transactions = 0;
		sim_time_type Sum_read_transaction_turnaround = 0;
		sim_time_type Sum_read_transaction_execution = 0;
		sim_time_type Sum_read_transaction_transfer = 0;
		sim_time_type Sum_read_transaction_waiting = 0;
		sim_time_type Sum_write_transaction_turnaround = 0;
		sim_time_type Sum_write_transaction_execution = 0;
		sim_time_type Sum_write_transaction_transfer = 0;
		sim_time_type Sum_write_transaction_waiting = 0;
		sim_time_type Sum_subrequest_latency = 0;
		sim_time_type Min_subrequest_latency = std::numeric_limits<sim_time_type>::max();
		sim_time_type Max_subrequest_latency = 0;
	};

	struct RAID_Request_Stats
	{
		uint64_t Submitted_requests = 0;
		uint64_t Completed_requests = 0;
		uint64_t Submitted_read_requests = 0;
		uint64_t Submitted_write_requests = 0;
		uint64_t Total_subrequests_dispatched = 0;
		uint64_t Submitted_read_sectors = 0;
		uint64_t Submitted_write_sectors = 0;
		uint64_t Completed_read_sectors = 0;
		uint64_t Completed_write_sectors = 0;
		sim_time_type Sum_request_completion_latency = 0;
		sim_time_type Min_request_completion_latency = std::numeric_limits<sim_time_type>::max();
		sim_time_type Max_request_completion_latency = 0;
		sim_time_type Sum_completion_skew = 0;
		sim_time_type Max_completion_skew = 0;
	};

	struct Swans_Stats
	{
		uint64_t Epoch_evaluations = 0;
		uint64_t Normal_epochs = 0;
		uint64_t Redirect_epochs = 0;
		uint64_t Migration_epochs = 0;
		uint64_t State_transitions = 0;
		uint64_t Redirect_operations = 0;
		uint64_t Migration_operations = 0;
		uint64_t Migration_barrier_waits = 0;
		uint64_t Buffered_requests = 0;
		uint64_t Buffered_write_requests = 0;
		uint64_t Buffered_write_sectors = 0;
		uint64_t Replay_requests = 0;
		uint64_t Buffered_write_completions = 0;
		uint64_t Background_read_ios = 0;
		uint64_t Background_write_ios = 0;
		uint64_t Source_discard_requests = 0;
		uint64_t Source_discard_sectors = 0;
		double Last_mu = 0.0;
	};

	struct Swans_Migration_Record
	{
		uint64_t Sequence = 0;
		sim_time_type Start_time = 0;
		uint64_t Hot_zone = 0;
		uint64_t Cold_zone = 0;
		unsigned int Hot_ssd = 0;
		unsigned int Cold_ssd = 0;
		unsigned int First_source_disk = 0;
		unsigned int First_destination_disk = 0;
		stream_id_type First_stream_id = 0;
		uint64_t Stream_count = 0;
		LHA_type First_source_lba = 0;
		LHA_type First_destination_lba = 0;
		uint64_t Copy_blocks = 0;
		uint64_t Copy_sectors = 0;
	};

	unsigned int ssd_count;
	unsigned int stripe_unit_lba;
	uint64_t swans_zone_size_lba;
	std::function<void(unsigned int, SSD_Components::User_Request*)> submit_callback;
	std::function<void(SSD_Components::User_Request*)> complete_callback;
	std::unordered_map<io_request_id_type, Inflight_Entry> inflight;
	std::unordered_map<io_request_id_type, Sub_Request_Metadata> subrequest_metadata_by_id;
	std::vector<SSD_Device*> backend_ssds;
	std::vector<Per_SSD_Stats> per_ssd_stats;
	RAID_Request_Stats raid_request_stats;

	bool swans_enabled;
	sim_time_type swans_epoch_default;
	sim_time_type swans_epoch_placement;
	sim_time_type swans_epoch_migration;
	sim_time_type swans_poll_interval;
	sim_time_type next_policy_evaluation_time;
	MQSimEngine::Sim_Event* swans_scheduled_event;
	sim_time_type swans_scheduled_event_time;
	RAID_Policy::PolicyState swans_policy_state;
	RAID_Policy::PolicyDecision swans_last_decision;
	RAID_Policy::ZoneDirectory zone_directory;
	RAID_Policy::WearLevelingPolicy wear_leveling_policy;
	RAID_Policy::MigrationExecutor migration_executor;
	Swans_Stats swans_stats;
	std::vector<Swans_Migration_Record> swans_migration_history;
	std::deque<SSD_Components::User_Request*> blocked_user_requests;

	SSD_Components::User_Request* Create_sub_request(const SSD_Components::User_Request* original, const RAID_Sub_Request& part) const;
	io_request_id_type Submit_background_copy(const RAID_Policy::StripeCopyPlan& copy, bool is_write, uint64_t task_index);
	bool Discard_migration_source(const RAID_Policy::StripeCopyPlan& copy, uint64_t task_index);
	void Observe_buffered_hot_write(const RAID_Policy::MigrationTask& task, const SSD_Components::User_Request* request);
	void Complete_buffered_user_request(SSD_Components::User_Request* request);
	void Try_replay_blocked_requests();
	unsigned int Swans_mapping_chunk_length(const RAID_Policy::ZoneResolveResult& resolved, unsigned int remaining) const;
	std::vector<uint64_t> Collect_zone_ids(LHA_type lba, unsigned int lba_count) const;
	bool Maybe_apply_redirect(uint64_t zone_id);
	std::vector<RAID_Policy::MigrationTask> Build_migration_tasks(const std::vector<RAID_Policy::MigrationOp>& ops) const;
	bool Has_inflight_user_io_on_migrating_zone() const;
	void Schedule_swans_event(sim_time_type fire_time);
	void Handle_swans_event();
	void process_new_user_request(SSD_Components::User_Request* user_request) override;
};

#endif
