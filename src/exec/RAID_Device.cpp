#include "RAID_Device.h"
#include <cmath>
#include <limits>
#include <map>
#include "../host/IO_Flow_Base.h"
#include "../host/SATA_HBA.h"
#include "../ssd/Host_Interface_NVMe.h"
#include "../ssd/Host_Interface_SATA.h"
#include "../ssd/FTL.h"
#include "../ssd/ONFI_Channel_Base.h"
#include "../utils/Logical_Address_Partitioning_Unit.h"
#include "../nvm_chip/flash_memory/Physical_Page_Address.h"

namespace {
	// Keep RAID result XML compact by default.
	// Enable these when deep diagnostics are needed.
	static const bool RAID_REPORT_INCLUDE_HOST_INTERFACE = true;
	static const bool RAID_REPORT_INCLUDE_PLANE_DETAILS = true;
	static const bool RAID_REPORT_INCLUDE_HISTOGRAM_BINS = true;
	static const bool RAID_REPORT_INCLUDE_BACKEND_SSD_DETAILS = true;

	struct Erase_Distribution_Summary
	{
		uint64_t Block_count = 0;
		uint64_t Sum_erase_count = 0;
		double Sum_square_erase_count = 0.0;
		unsigned int Min_erase_count = std::numeric_limits<unsigned int>::max();
		unsigned int Max_erase_count = 0;
	};

	void update_erase_summary(Erase_Distribution_Summary& summary, unsigned int erase_count)
	{
		summary.Block_count++;
		summary.Sum_erase_count += erase_count;
		summary.Sum_square_erase_count += (double)erase_count * (double)erase_count;
		if (erase_count < summary.Min_erase_count) {
			summary.Min_erase_count = erase_count;
		}
		if (erase_count > summary.Max_erase_count) {
			summary.Max_erase_count = erase_count;
		}
	}

	double erase_avg(const Erase_Distribution_Summary& summary)
	{
		return summary.Block_count == 0 ? 0.0 : (double)summary.Sum_erase_count / (double)summary.Block_count;
	}

	double erase_stddev(const Erase_Distribution_Summary& summary)
	{
		if (summary.Block_count == 0) {
			return 0.0;
		}
		double avg = erase_avg(summary);
		double variance = (summary.Sum_square_erase_count / (double)summary.Block_count) - (avg * avg);
		if (variance < 0) {
			variance = 0;
		}
		return std::sqrt(variance);
	}

	unsigned int erase_min_or_zero(const Erase_Distribution_Summary& summary)
	{
		return summary.Block_count == 0 ? 0 : summary.Min_erase_count;
	}
}

RAID_Device::RAID_Device(Device_Parameter_Set* parameters, std::vector<IO_Flow_Parameter_Set*>* io_flows)
	: MQSimEngine::Sim_Object("RAIDDevice")
{
	Simulator->AddObject(this);
	ssd_count = parameters->SSD_Count > 0 ? parameters->SSD_Count : 1;  // SSD 개수 저장
	stripe_unit_lba = parameters->Stripe_Unit_LBA;  // 스트라이프 크기 저장
	ssd_configs = std::vector<Device_Parameter_Set>(ssd_count, *parameters);
	ssds.reserve(ssd_count);  // SSD 개수만큼 메모리 할당
	for (unsigned int i = 0; i < ssd_count; i++) {  // SSD 개수만큼 반복
		ssds.push_back(new SSD_Device(&ssd_configs[i], io_flows, "SSDDevice_" + std::to_string(i))); // 각 SSD는 SSDDevice_i 이름으로 생성
		ssds.back()->Host_interface->Set_internal_submission(true);
	}
	LHA_type per_ssd_lha_count = Utils::Logical_Address_Partitioning_Unit::Get_total_device_lha_count();
	LHA_type raid_visible_lha_count = per_ssd_lha_count;
	if (ssd_count > 1 && per_ssd_lha_count > 0) {
		if (per_ssd_lha_count > std::numeric_limits<LHA_type>::max() / ssd_count) {
			raid_visible_lha_count = std::numeric_limits<LHA_type>::max();
		} else {
			raid_visible_lha_count = per_ssd_lha_count * ssd_count;
		}
	}

	raid_controller = new RAID_Controller(ID() + ".RAIDController", nullptr, (unsigned int)io_flows->size(), ssd_count, parameters->Stripe_Unit_LBA);
	Simulator->AddObject(raid_controller); // 시뮬레이터에 등록

	switch (parameters->HostInterface_Type) {
		case HostInterface_Types::NVME:
			Host_interface = new SSD_Components::Host_Interface_NVMe(ID() + ".HostInterface",
				raid_visible_lha_count, parameters->IO_Queue_Depth, parameters->IO_Queue_Depth,
				(unsigned int)io_flows->size(), parameters->Queue_Fetch_Size, parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, raid_controller);
			break;
		case HostInterface_Types::SATA:
			Host_interface = new SSD_Components::Host_Interface_SATA(ID() + ".HostInterface",
				parameters->IO_Queue_Depth, raid_visible_lha_count,
				parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, raid_controller);
			break;
		default:
			Host_interface = nullptr;
			break;
	}
	if (Host_interface != nullptr) {
		Simulator->AddObject(Host_interface);
		Host_interface->Set_segmentation_enabled(false);
	}
	raid_controller->Set_host_interface(Host_interface);
	raid_controller->Set_backend_ssds(ssds);
	for (unsigned int ssd_idx = 0; ssd_idx < ssds.size(); ssd_idx++) {
		SSD_Device* ssd = ssds[ssd_idx];
		ssd->Cache_manager->Connect_to_user_request_serviced_signal([this](SSD_Components::User_Request* sub_request) {
			this->raid_controller->Notify_sub_request_completed(sub_request);
		});
		ssd->Cache_manager->Connect_to_user_memory_transaction_serviced_signal([this, ssd_idx](SSD_Components::NVM_Transaction* transaction) {
			this->raid_controller->Notify_sub_transaction_completed(ssd_idx, transaction);
		});
	} // 각 SSD의 캐시 매니저에 완료 알림 등록
}

RAID_Device::~RAID_Device()
{
	delete Host_interface;
	delete raid_controller;
	for (auto &ssd : ssds) {
		delete ssd;
	}
}

void RAID_Device::Attach_to_host(Host_Components::PCIe_Switch* pcie_switch)
{
	if (Host_interface != nullptr) {
		Host_interface->Attach_to_device(pcie_switch); // 호스트 인터페이스에 피어 연결
	}
}

void RAID_Device::Perform_preconditioning(std::vector<Utils::Workload_Statistics*> workload_stats)
{
	for (auto &ssd : ssds) {
		ssd->Perform_preconditioning(workload_stats);  // 각 SSD에 대해 워밍업 수행
	}
} 

void RAID_Device::Initialize_io_streams(const std::vector<Host_Components::IO_Flow_Base*>& io_flows,
	Host_Components::SATA_HBA* sata_hba)
{
	switch (Host_interface->GetType()) { // 호스트 인터페이스 타입에 따라 처리
		case HostInterface_Types::NVME:
			for (uint16_t flow_cntr = 0; flow_cntr < io_flows.size(); flow_cntr++) {
				((SSD_Components::Host_Interface_NVMe*) Host_interface)->Create_new_stream(
					io_flows[flow_cntr]->Priority_class(),
					io_flows[flow_cntr]->Get_start_lsa_on_device(), io_flows[flow_cntr]->Get_end_lsa_address_on_device(),
					io_flows[flow_cntr]->Get_nvme_queue_pair_info()->Submission_queue_memory_base_address,
					io_flows[flow_cntr]->Get_nvme_queue_pair_info()->Completion_queue_memory_base_address);
			}
			break;
		case HostInterface_Types::SATA:
			if (sata_hba != NULL) {
				((SSD_Components::Host_Interface_SATA*) Host_interface)->Set_ncq_address(
					sata_hba->Get_sata_ncq_info()->Submission_queue_memory_base_address,
					sata_hba->Get_sata_ncq_info()->Completion_queue_memory_base_address);
			}
			break;
		default:
			break;
	}

	for (auto &ssd : ssds) {
		switch (ssd->Host_interface->GetType()) {
			case HostInterface_Types::NVME:
				for (uint16_t flow_cntr = 0; flow_cntr < io_flows.size(); flow_cntr++) {
					LHA_type start_lha = Utils::Logical_Address_Partitioning_Unit::Start_lha_available_to_flow(flow_cntr);
					LHA_type end_lha = Utils::Logical_Address_Partitioning_Unit::End_lha_available_to_flow(flow_cntr);
					((SSD_Components::Host_Interface_NVMe*) ssd->Host_interface)->Create_new_stream(
						io_flows[flow_cntr]->Priority_class(),
						start_lha, end_lha,
						0, 0);
				}
				break;
			case HostInterface_Types::SATA:
				((SSD_Components::Host_Interface_SATA*) ssd->Host_interface)->Set_ncq_address(0, 0);
				break;
			default:
				break;
		}
	}
}

void RAID_Device::Start_simulation()
{
}

void RAID_Device::Validate_simulation_config()
{
}

void RAID_Device::Execute_simulator_event(MQSimEngine::Sim_Event* event)
{
}

void RAID_Device::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter) // Host_interface와 각 SSD들의 결과를 XML로 저장
{
	std::string tmp = name_prefix + ".RAIDDevice";
	xmlwriter.Write_open_tag(tmp);
	if (Host_interface != nullptr && RAID_REPORT_INCLUDE_HOST_INTERFACE) {
		Host_interface->Report_results_in_XML(tmp, xmlwriter);
	}
	if (raid_controller != nullptr) {
		raid_controller->Report_results_in_XML(tmp, xmlwriter);
	}
	{
		std::string wl_tag = tmp + ".WearLeveling";
		xmlwriter.Write_open_tag(wl_tag);
		unsigned long long total_flash_page_programs_all_ssds = 0;
		unsigned long long total_flash_page_erases_all_ssds = 0;
		unsigned long long total_host_write_bytes_dispatched_all_ssds = 0;
		unsigned long long total_flash_programmed_bytes_all_ssds = 0;
		std::map<unsigned int, uint64_t> raid_erase_histogram;
		for (unsigned int ssd_idx = 0; ssd_idx < ssds.size(); ssd_idx++) {
			SSD_Device* ssd = ssds[ssd_idx];
			if (ssd == nullptr || ssd->Memory_Type != NVM::NVM_Type::FLASH || ssd->Firmware == nullptr) {
				continue;
			}
			SSD_Components::FTL* ftl = static_cast<SSD_Components::FTL*>(ssd->Firmware);
			if (ftl->BlockManager == nullptr) {
				continue;
			}

			Erase_Distribution_Summary ssd_summary;
			std::map<unsigned int, uint64_t> ssd_erase_histogram;
			unsigned int plane_count = 0;
			unsigned int worst_plane_spread = 0;
			double sum_plane_spread = 0.0;
			unsigned long long ssd_flash_page_programs = 0;
			unsigned long long ssd_flash_page_erases = 0;

			const Device_Parameter_Set& cfg = ssd_configs[ssd_idx];
			for (unsigned int ch = 0; ch < ssd->Channel_count; ch++) {
				SSD_Components::ONFI_Channel_Base* channel = static_cast<SSD_Components::ONFI_Channel_Base*>(ssd->Channels[ch]);
				for (unsigned int chip = 0; chip < ssd->Chip_no_per_channel; chip++) {
					NVM::FlashMemory::Flash_Chip* flash_chip = channel->Chips[chip];
					if (flash_chip != nullptr) {
						ssd_flash_page_programs += flash_chip->Get_total_plane_program_count();
						ssd_flash_page_erases += flash_chip->Get_total_plane_erase_count();
					}
					for (unsigned int die = 0; die < cfg.Flash_Parameters.Die_No_Per_Chip; die++) {
						for (unsigned int plane = 0; plane < cfg.Flash_Parameters.Plane_No_Per_Die; plane++) {
							NVM::FlashMemory::Physical_Page_Address plane_address(ch, chip, die, plane, 0, 0);
							SSD_Components::PlaneBookKeepingType* pbke = ftl->BlockManager->Get_plane_bookkeeping_entry(plane_address);
							if (pbke == nullptr || pbke->Blocks == nullptr) {
								continue;
							}

							Erase_Distribution_Summary plane_summary;
								for (unsigned int block = 0; block < cfg.Flash_Parameters.Block_No_Per_Plane; block++) {
									unsigned int erase_count = pbke->Blocks[block].Erase_count;
									update_erase_summary(plane_summary, erase_count);
									update_erase_summary(ssd_summary, erase_count);
									ssd_erase_histogram[erase_count]++;
									raid_erase_histogram[erase_count]++;
								}

							unsigned int plane_spread = plane_summary.Block_count == 0 ? 0 : (plane_summary.Max_erase_count - erase_min_or_zero(plane_summary));
							if (plane_spread > worst_plane_spread) {
								worst_plane_spread = plane_spread;
							}
							sum_plane_spread += plane_spread;
							plane_count++;

							if (RAID_REPORT_INCLUDE_PLANE_DETAILS) {
								std::string plane_tag = wl_tag + ".Plane";
								xmlwriter.Write_open_tag(plane_tag);
								xmlwriter.Write_attribute_string("SSD_ID", std::to_string(ssd_idx));
								xmlwriter.Write_attribute_string("Channel", std::to_string(ch));
								xmlwriter.Write_attribute_string("Chip", std::to_string(chip));
								xmlwriter.Write_attribute_string("Die", std::to_string(die));
								xmlwriter.Write_attribute_string("Plane", std::to_string(plane));
								xmlwriter.Write_attribute_string("Block_Count", std::to_string(plane_summary.Block_count));
								xmlwriter.Write_attribute_string("Min_Block_Erase_Count", std::to_string(erase_min_or_zero(plane_summary)));
								xmlwriter.Write_attribute_string("Max_Block_Erase_Count", std::to_string(plane_summary.Max_erase_count));
								xmlwriter.Write_attribute_string("Avg_Block_Erase_Count", std::to_string(erase_avg(plane_summary)));
								xmlwriter.Write_attribute_string("StdDev_Block_Erase_Count", std::to_string(erase_stddev(plane_summary)));
								xmlwriter.Write_attribute_string("Erase_Count_Spread", std::to_string(plane_spread));
								xmlwriter.Write_close_tag();
							}
						}
					}
				}
			}
			unsigned long long host_write_bytes_to_ssd = raid_controller == nullptr ? 0 :
				(unsigned long long)raid_controller->Get_ssd_subrequest_write_sectors(ssd_idx) * (unsigned long long)SECTOR_SIZE_IN_BYTE;
			unsigned long long flash_programmed_bytes_to_ssd = ssd_flash_page_programs * (unsigned long long)cfg.Flash_Parameters.Page_Capacity;
			double approx_flash_wa = host_write_bytes_to_ssd == 0 ? 0.0 :
				(double)flash_programmed_bytes_to_ssd / (double)host_write_bytes_to_ssd;
			total_flash_page_programs_all_ssds += ssd_flash_page_programs;
			total_flash_page_erases_all_ssds += ssd_flash_page_erases;
			total_host_write_bytes_dispatched_all_ssds += host_write_bytes_to_ssd;
			total_flash_programmed_bytes_all_ssds += flash_programmed_bytes_to_ssd;

			std::string ssd_tag = wl_tag + ".SSD";
			xmlwriter.Write_open_tag(ssd_tag);
			xmlwriter.Write_attribute_string("SSD_ID", std::to_string(ssd_idx));
			xmlwriter.Write_attribute_string("Plane_Count", std::to_string(plane_count));
			xmlwriter.Write_attribute_string("Block_Count", std::to_string(ssd_summary.Block_count));
			xmlwriter.Write_attribute_string("Min_Block_Erase_Count", std::to_string(erase_min_or_zero(ssd_summary)));
			xmlwriter.Write_attribute_string("Max_Block_Erase_Count", std::to_string(ssd_summary.Max_erase_count));
			xmlwriter.Write_attribute_string("Avg_Block_Erase_Count", std::to_string(erase_avg(ssd_summary)));
			xmlwriter.Write_attribute_string("StdDev_Block_Erase_Count", std::to_string(erase_stddev(ssd_summary)));
			xmlwriter.Write_attribute_string("Erase_Count_Spread", std::to_string(ssd_summary.Block_count == 0 ? 0 : (ssd_summary.Max_erase_count - erase_min_or_zero(ssd_summary))));
			xmlwriter.Write_attribute_string("Worst_Plane_Erase_Spread", std::to_string(worst_plane_spread));
			xmlwriter.Write_attribute_string("Average_Plane_Erase_Spread", std::to_string(plane_count == 0 ? 0.0 : sum_plane_spread / (double)plane_count));
			xmlwriter.Write_attribute_string("Flash_Page_Program_Count", std::to_string(ssd_flash_page_programs));
			xmlwriter.Write_attribute_string("Flash_Page_Erase_Count", std::to_string(ssd_flash_page_erases));
			xmlwriter.Write_attribute_string("Host_Write_Bytes_Dispatched", std::to_string(host_write_bytes_to_ssd));
			xmlwriter.Write_attribute_string("Approx_Flash_Programmed_Bytes", std::to_string(flash_programmed_bytes_to_ssd));
			xmlwriter.Write_attribute_string("Approx_Flash_Write_Amplification", std::to_string(approx_flash_wa));
			xmlwriter.Write_close_tag();

			std::string hist_tag = wl_tag + ".EraseHistogram.SSD";
			xmlwriter.Write_open_tag(hist_tag);
			xmlwriter.Write_attribute_string("SSD_ID", std::to_string(ssd_idx));
			xmlwriter.Write_attribute_string("Block_Count", std::to_string(ssd_summary.Block_count));
			if (RAID_REPORT_INCLUDE_HISTOGRAM_BINS) {
				for (const auto& bin : ssd_erase_histogram) {
					std::string bin_tag = hist_tag + ".Bin";
					xmlwriter.Write_open_tag(bin_tag);
					xmlwriter.Write_attribute_string("Erase_Count", std::to_string(bin.first));
					xmlwriter.Write_attribute_string("Block_Count", std::to_string(bin.second));
					xmlwriter.Write_attribute_string("Fraction", std::to_string(ssd_summary.Block_count == 0 ? 0.0 : (double)bin.second / (double)ssd_summary.Block_count));
					xmlwriter.Write_close_tag();
				}
			}
			xmlwriter.Write_close_tag();
		}
		std::string wa_tag = wl_tag + ".RAIDWriteAmplification";
		xmlwriter.Write_open_tag(wa_tag);
		uint64_t host_write_sectors_total = raid_controller == nullptr ? 0 : raid_controller->Get_total_host_write_sectors();
		uint64_t subreq_write_sectors_total = raid_controller == nullptr ? 0 : raid_controller->Get_total_subrequest_write_sectors();
		unsigned long long host_write_bytes_total = (unsigned long long)host_write_sectors_total * (unsigned long long)SECTOR_SIZE_IN_BYTE;
		double logical_raid_wa = host_write_sectors_total == 0 ? 0.0 : (double)subreq_write_sectors_total / (double)host_write_sectors_total;
		double approx_flash_wa_total = total_host_write_bytes_dispatched_all_ssds == 0 ? 0.0 :
			(double)total_flash_programmed_bytes_all_ssds / (double)total_host_write_bytes_dispatched_all_ssds;
		xmlwriter.Write_attribute_string("Host_Write_Sectors", std::to_string(host_write_sectors_total));
		xmlwriter.Write_attribute_string("SubRequest_Write_Sectors", std::to_string(subreq_write_sectors_total));
		xmlwriter.Write_attribute_string("Logical_RAID_Write_Amplification", std::to_string(logical_raid_wa));
		xmlwriter.Write_attribute_string("Host_Write_Bytes", std::to_string(host_write_bytes_total));
		xmlwriter.Write_attribute_string("Host_Write_Bytes_Dispatched_To_SSDs", std::to_string(total_host_write_bytes_dispatched_all_ssds));
		xmlwriter.Write_attribute_string("Approx_Flash_Page_Programs_All_SSDs", std::to_string(total_flash_page_programs_all_ssds));
		xmlwriter.Write_attribute_string("Approx_Flash_Page_Erases_All_SSDs", std::to_string(total_flash_page_erases_all_ssds));
		xmlwriter.Write_attribute_string("Approx_Flash_Programmed_Bytes_All_SSDs", std::to_string(total_flash_programmed_bytes_all_ssds));
		xmlwriter.Write_attribute_string("Approx_Flash_Write_Amplification", std::to_string(approx_flash_wa_total));
		xmlwriter.Write_close_tag();

		std::string raid_hist_tag = wl_tag + ".EraseHistogram.RAID";
		xmlwriter.Write_open_tag(raid_hist_tag);
		if (RAID_REPORT_INCLUDE_HISTOGRAM_BINS) {
			for (const auto& bin : raid_erase_histogram) {
				std::string bin_tag = raid_hist_tag + ".Bin";
				xmlwriter.Write_open_tag(bin_tag);
				xmlwriter.Write_attribute_string("Erase_Count", std::to_string(bin.first));
				xmlwriter.Write_attribute_string("Block_Count", std::to_string(bin.second));
				xmlwriter.Write_close_tag();
			}
		}
		xmlwriter.Write_close_tag();
		xmlwriter.Write_close_tag();
	}
	if (RAID_REPORT_INCLUDE_BACKEND_SSD_DETAILS) {
		for (unsigned int i = 0; i < ssds.size(); i++) {
			ssds[i]->Report_results_in_XML(tmp + ".SSD_" + std::to_string(i), xmlwriter);
		}
	}
	xmlwriter.Write_close_tag();
}

unsigned int RAID_Device::Get_no_of_LHAs_in_an_NVM_write_unit()
{
	return Host_interface->Get_no_of_LHAs_in_an_NVM_write_unit();
}

LPA_type RAID_Device::Convert_host_logical_address_to_device_address(LHA_type lha)
{
	unsigned int disk_id = 0;
	LHA_type physical_lba = 0;
	if (ssd_count == 0 || stripe_unit_lba == 0) {
		disk_id = 0;
		physical_lba = lha;
	} else {
		LHA_type stripe_index = lha / stripe_unit_lba;
		disk_id = static_cast<unsigned int>(stripe_index % ssd_count);
		LHA_type stripe_row = stripe_index / ssd_count;
		LHA_type in_stripe_offset = lha % stripe_unit_lba;
		physical_lba = stripe_row * stripe_unit_lba + in_stripe_offset;
	}
	if (disk_id >= ssds.size()) {
		return 0;
	}
	return ssds[disk_id]->Convert_host_logical_address_to_device_address(physical_lba);
}

page_status_type RAID_Device::Find_NVM_subunit_access_bitmap(LHA_type lha)
{
	unsigned int disk_id = 0;
	LHA_type physical_lba = 0;
	if (ssd_count == 0 || stripe_unit_lba == 0) {
		disk_id = 0;
		physical_lba = lha;
	} else {
		LHA_type stripe_index = lha / stripe_unit_lba;
		disk_id = static_cast<unsigned int>(stripe_index % ssd_count);
		LHA_type stripe_row = stripe_index / ssd_count;
		LHA_type in_stripe_offset = lha % stripe_unit_lba;
		physical_lba = stripe_row * stripe_unit_lba + in_stripe_offset;
	}
	if (disk_id >= ssds.size()) {
		return 0;
	}
	return ssds[disk_id]->Find_NVM_subunit_access_bitmap(physical_lba);
}
