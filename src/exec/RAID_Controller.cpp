#include "RAID_Controller.h"
#include <iostream>		// 디버그/경고 로그 출력용
#include <limits>
#include <numeric>
#include "../sim/Sim_Defs.h"
#include "../utils/XMLWriter.h"
#include "SSD_Device.h"

// 디버그 로그 활성화 여부 (문제 발생 시에만 true로 설정)
static const bool ENABLE_RAID_DEBUG_LOG = false;
static const bool ENABLE_RAID_WARN_LOG = true;  // WARN은 항상 켜둠

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
}

RAID_Controller::RAID_Controller(const sim_object_id_type& id, SSD_Components::Host_Interface_Base* host_interface,
	unsigned int stream_count, unsigned int ssd_count, unsigned int stripe_unit_lba)
	: SSD_Components::Data_Cache_Manager_Base(id, host_interface, nullptr, 1, 1, 1, 0, 0, 0, nullptr,
			SSD_Components::Cache_Sharing_Mode::SHARED, stream_count),
		ssd_count(ssd_count), stripe_unit_lba(stripe_unit_lba)
{
	per_ssd_stats.resize(ssd_count);
	Set_complete_callback([this](SSD_Components::User_Request* request) {
		if (request->Transaction_list.size() != 0) {
			std::cerr << "[RAID] WARN: Completing request with non-empty Transaction_list! ID=" << request->ID
			          << " Transactions=" << request->Transaction_list.size() << std::endl;
		}
		this->broadcast_user_request_serviced_signal(request);
	});
} // RAID_Controller 생성자로 구성 파라미터 (SSD개수, 스트라이프 유닛 크기)	설정

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
} // sub-request를 실제 SSD로 보낼 때 사용할 콜백 등록

void RAID_Controller::Set_complete_callback(std::function<void(SSD_Components::User_Request*)> callback)
{
	complete_callback = callback;
} // 모든 sub-request가 완료되었을 때 사용할 콜백 등록

void RAID_Controller::Submit(SSD_Components::User_Request* request)
{ // 원본 요청을 SSD에 보내는 함수
	sim_time_type now = Simulator->Time();
	const sim_time_type request_start = request_start_time(request, now);
	std::vector<RAID_Sub_Request> parts = Split(request->Start_LBA, request->SizeInSectors); //split가 나눈 요청을 RAID_Sub_Request로 구성된 parts라는 벡터에 저장
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
	inflight[request->ID] = entry; // 원본 요청 ID에 대한 inflight 엔트리 생성 및 해당 요청에 대한 대기 중인 sub-request 수 저장

	if (ENABLE_RAID_DEBUG_LOG) {
		std::cerr << "[RAID] Submit original request ID=" << request->ID
		          << " Stream=" << (int)request->Stream_id
		          << " Type=" << (request->Type == SSD_Components::UserRequestType::READ ? "READ" : "WRITE")
		          << " Start_LBA=" << request->Start_LBA
		          << " LBA_count=" << request->SizeInSectors
		          << " Parts=" << parts.size()
		          << std::endl;
	}

	if (!submit_callback) { // 콜백이 없으면 종료
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
	for (auto &part : parts) {
		SSD_Components::User_Request* sub_request = Create_sub_request(request, part); // 원본 요청과 sub정보를 받아 새로운 객체 생성
		Sub_Request_Metadata metadata;
		metadata.Parent = request;
		metadata.Disk_id = part.Disk_id;
		metadata.Size_in_sectors = part.LBA_count;
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
			}
		}
		if (ENABLE_RAID_DEBUG_LOG) {
			std::cerr << "[RAID]  Sub-request ID=" << sub_request->ID
			          << " Parent=" << request->ID
			          << " Disk=" << part.Disk_id
			          << " Start_LBA=" << part.Start_LBA
			          << " LBA_count=" << part.LBA_count
			          << std::endl;
		}
		submit_callback(part.Disk_id, sub_request);
	}
}

void RAID_Controller::Notify_sub_request_completed(SSD_Components::User_Request* sub_request)
{
	sim_time_type now = Simulator->Time();
	auto metadata_it = subrequest_metadata_by_id.find(sub_request->ID);
	if (metadata_it == subrequest_metadata_by_id.end()) {
		if (ENABLE_RAID_WARN_LOG) {
			std::cerr << "[RAID]  WARN: completion for unknown sub_request ID=" << sub_request->ID << std::endl;
		}
		return;
	}
	Sub_Request_Metadata metadata = metadata_it->second;
	subrequest_metadata_by_id.erase(metadata_it);
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

	auto it = inflight.find(original_request->ID); // original_request의 ID를 찾아 it에 저장
	if (it == inflight.end()) { // it이 inflight의 끝을 가리키면 종료
		if (ENABLE_RAID_WARN_LOG) {
			std::cerr << "[RAID]  WARN: inflight entry missing for parent ID=" << original_request->ID
			          << " (sub_request ID=" << sub_request->ID << ")" << std::endl;
		}
		return;
	}

	unsigned int pending_before = it->second.Pending;
	if (it->second.Pending > 0) { // it->second.Pending이 0보다 크면 1 감소
		it->second.Pending--;
	}
	if (sub_latency < it->second.Min_subrequest_latency) {
		it->second.Min_subrequest_latency = sub_latency;
	}
	if (sub_latency > it->second.Max_subrequest_latency) {
		it->second.Max_subrequest_latency = sub_latency;
	}
	if (ENABLE_RAID_DEBUG_LOG) {
		std::cerr << "[RAID]  Complete sub_request ID=" << sub_request->ID
		          << " Parent=" << original_request->ID
		          << " Pending(before)=" << pending_before
		          << " Pending(after)=" << it->second.Pending
		          << std::endl;
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

		inflight.erase(it); // it을 삭제
		if (ENABLE_RAID_DEBUG_LOG) {
			std::cerr << "[RAID]  All sub-requests completed for Parent=" << original_request->ID << std::endl;
		}
		// 원본 요청의 Transaction_list가 비어있는지 확인 (RAID에서는 세그먼트화되지 않으므로 비어있어야 함)
		if (original_request->Transaction_list.size() != 0) {
			std::cerr << "[RAID]  WARN: Original request has non-empty Transaction_list! ID=" << original_request->ID
			          << " Transactions=" << original_request->Transaction_list.size() << std::endl;
		}
		if (complete_callback) {
			complete_callback(original_request); // 원본 요청 완료 콜백 호출
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

void RAID_Controller::Map(LHA_type lba, unsigned int& disk_id, LHA_type& physical_lba) const
{
	if (ssd_count == 0 || stripe_unit_lba == 0) {
		disk_id = 0;
		physical_lba = lba;
		return;
	}
	LHA_type stripe_index = lba / stripe_unit_lba; // 몇번째 스트라이프인지 계산
	disk_id = static_cast<unsigned int>(stripe_index % ssd_count); // 몇번째 디스크인지 계산
	LHA_type stripe_row = stripe_index / ssd_count;  // 몇번째 행인지 계산
	LHA_type in_stripe_offset = lba % stripe_unit_lba; // 몇번째 오프셋인지 계산
	physical_lba = stripe_row * stripe_unit_lba + in_stripe_offset; // 물리적 주소 계산
}

std::vector<RAID_Sub_Request> RAID_Controller::Split(LHA_type lba, unsigned int lba_count) const
{
	std::vector<RAID_Sub_Request> parts; //리스트 생성
	if (lba_count == 0 || ssd_count == 0 || stripe_unit_lba == 0) {
		if (lba_count > 0) {
			parts.push_back({ 0, lba, lba_count });
		}
		return parts;
	}

	LHA_type current_lba = lba;
	unsigned int remaining = lba_count; // 아직 처리해야 할 섹터 수
	while (remaining > 0) {
		unsigned int disk_id = 0;
		LHA_type physical_lba = 0;

		// RAID0 공식으로 disk_id와 physical_lba 계산
		Map(current_lba, disk_id, physical_lba);
		unsigned int in_stripe_offset = static_cast<unsigned int>(current_lba % stripe_unit_lba); // 스트라이프 내부에서의 오프셋
		unsigned int stripe_remaining = stripe_unit_lba - in_stripe_offset; // 스트라이프 내부에서 남은 섹터 수
		unsigned int chunk = remaining < stripe_remaining ? remaining : stripe_remaining; 
		parts.push_back({ disk_id, physical_lba, chunk });
		current_lba += chunk;
		remaining -= chunk;
	}
	return parts;
}

SSD_Components::User_Request* RAID_Controller::Create_sub_request(const SSD_Components::User_Request* original, const RAID_Sub_Request& part) const
{
	SSD_Components::User_Request* sub_request = new SSD_Components::User_Request; //sub_request 생성
	sub_request->Priority_class = original->Priority_class; 
	sub_request->Start_LBA = part.Start_LBA; //조각 시작 = part.Start_LBA
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

void RAID_Controller::Do_warmup(std::vector<Utils::Workload_Statistics*>)
{
}

void RAID_Controller::Execute_simulator_event(MQSimEngine::Sim_Event*)
{
}

void RAID_Controller::process_new_user_request(SSD_Components::User_Request* user_request)
{
	Submit(user_request);
}

void RAID_Controller::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter) const
{
	std::string tag = name_prefix + ".RAIDController";
	xmlwriter.Write_open_tag(tag);

	xmlwriter.Write_attribute_string("SSD_Count", std::to_string(ssd_count));
	xmlwriter.Write_attribute_string("Stripe_Unit_LBA", std::to_string(stripe_unit_lba));
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

	xmlwriter.Write_close_tag();
}

uint64_t RAID_Controller::Get_total_host_write_sectors() const
{
	return raid_request_stats.Submitted_write_sectors;
}

uint64_t RAID_Controller::Get_total_subrequest_write_sectors() const
{
	uint64_t total = 0;
	for (const auto& stats : per_ssd_stats) {
		total += stats.Submitted_write_sectors;
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
