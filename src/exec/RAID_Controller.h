#ifndef RAID_CONTROLLER_H
#define RAID_CONTROLLER_H

#include <vector>
#include <unordered_map>
#include <functional>
#include <limits>
#include "../ssd/Data_Cache_Manager_Base.h"
#include "../ssd/SSD_Defs.h"
#include "../ssd/User_Request.h"

class SSD_Device;
namespace Utils { class XmlWriter; }

struct RAID_Sub_Request // RAID0으로 split된 요청
{
	unsigned int Disk_id;
	LHA_type Start_LBA;
	unsigned int LBA_count;
};

class RAID_Controller : public SSD_Components::Data_Cache_Manager_Base // RAID0 분할/집계 + HostInterface 어댑터
{
public:
	RAID_Controller(const sim_object_id_type& id, SSD_Components::Host_Interface_Base* host_interface,
		unsigned int stream_count, unsigned int ssd_count, unsigned int stripe_unit_lba);
	void Set_backend_ssds(const std::vector<SSD_Device*>& ssds);
	void Set_submit_callback(std::function<void(unsigned int, SSD_Components::User_Request*)> callback); // 실제 SSD로 전달하는 콜백 함수
	void Set_complete_callback(std::function<void(SSD_Components::User_Request*)> callback); // 모든 sub-request 완료 후 원 요청 완료 알리는 콜백 함수

	void Submit(SSD_Components::User_Request* request); 
	void Notify_sub_request_completed(SSD_Components::User_Request* sub_request); // sub-request 완료 알리는 함수
	void Notify_sub_transaction_completed(unsigned int disk_id, SSD_Components::NVM_Transaction* transaction);
	void Map(LHA_type lba, unsigned int& disk_id, LHA_type& physical_lba) const; // 물리적 주소 계산
	std::vector<RAID_Sub_Request> Split(LHA_type lba, unsigned int lba_count) const; // 분할
	void Do_warmup(std::vector<Utils::Workload_Statistics*> workload_stats) override;
	void Execute_simulator_event(MQSimEngine::Sim_Event* event) override;
	void Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter) const;
	uint64_t Get_total_host_write_sectors() const;
	uint64_t Get_total_subrequest_write_sectors() const;
	uint64_t Get_ssd_subrequest_write_sectors(unsigned int disk_id) const;

private:
	struct Inflight_Entry // 남은 sub-request 수 저장
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
		SSD_Components::UserRequestType Type = SSD_Components::UserRequestType::READ;
		sim_time_type Submit_time = 0;
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

	unsigned int ssd_count;
	unsigned int stripe_unit_lba;
	std::function<void(unsigned int, SSD_Components::User_Request*)> submit_callback;
	std::function<void(SSD_Components::User_Request*)> complete_callback;
	std::unordered_map<io_request_id_type, Inflight_Entry> inflight;
	std::unordered_map<io_request_id_type, Sub_Request_Metadata> subrequest_metadata_by_id;
	std::vector<SSD_Device*> backend_ssds;
	std::vector<Per_SSD_Stats> per_ssd_stats;
	RAID_Request_Stats raid_request_stats;

	SSD_Components::User_Request* Create_sub_request(const SSD_Components::User_Request* original, const RAID_Sub_Request& part) const;
	void process_new_user_request(SSD_Components::User_Request* user_request) override;
};

#endif
