#include <vector>
#include <stdexcept>
#include <ctime>
#include <cmath>
#include <limits>
#include <map>
#include "SSD_Device.h"
#include "../ssd/ONFI_Channel_Base.h"
#include "../ssd/FTL.h"
#include "../ssd/Flash_Block_Manager.h"
#include "../ssd/Data_Cache_Manager_Flash_Advanced.h"
#include "../ssd/Data_Cache_Manager_Flash_Simple.h"
#include "../ssd/Address_Mapping_Unit_Base.h"
#include "../ssd/Address_Mapping_Unit_Page_Level.h"
#include "../ssd/Address_Mapping_Unit_Hybrid.h"
#include "../ssd/GC_and_WL_Unit_Page_Level.h"
#include "../ssd/TSU_OutofOrder.h"
#include "../ssd/TSU_Priority_OutOfOrder.h"
#include "../ssd/TSU_FLIN.h"
#include "../ssd/ONFI_Channel_NVDDR2.h"
#include "../ssd/NVM_PHY_ONFI_NVDDR2.h"
#include "../utils/Logical_Address_Partitioning_Unit.h"
#include "../nvm_chip/flash_memory/Physical_Page_Address.h"

namespace {
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

SSD_Device *SSD_Device::my_instance; //Used in static functions

SSD_Device::SSD_Device(Device_Parameter_Set *parameters, std::vector<IO_Flow_Parameter_Set *> *io_flows) : MQSimEngine::Sim_Object("SSDDevice")
{
	SSD_Device *device = this;
	my_instance = device; //used for static functions
	Simulator->AddObject(device);

	device->Preconditioning_required = parameters->Enabled_Preconditioning;
	device->Memory_Type = parameters->Memory_Type;
	device->Die_no_per_chip = parameters->Flash_Parameters.Die_No_Per_Chip;
	device->Plane_no_per_die = parameters->Flash_Parameters.Plane_No_Per_Die;
	device->Block_no_per_plane = parameters->Flash_Parameters.Block_No_Per_Plane;
	device->Page_no_per_block = parameters->Flash_Parameters.Page_No_Per_Block;
	device->Page_capacity_bytes = parameters->Flash_Parameters.Page_Capacity;

	switch (Memory_Type)
	{
	case NVM::NVM_Type::FLASH:
	{
		sim_time_type *read_latencies, *write_latencies;
		sim_time_type average_flash_read_latency = 0, average_flash_write_latency = 0; //Required for FTL initialization

		//Step 1: create memory chips (flash chips in our case)
		switch (parameters->Flash_Parameters.Flash_Technology)
		{
		case Flash_Technology_Type::SLC:
			read_latencies = new sim_time_type[1];
			read_latencies[0] = parameters->Flash_Parameters.Page_Read_Latency_LSB;
			write_latencies = new sim_time_type[1];
			write_latencies[0] = parameters->Flash_Parameters.Page_Program_Latency_LSB;
			average_flash_read_latency = read_latencies[0];
			average_flash_write_latency = write_latencies[0];
			break;
		case Flash_Technology_Type::MLC:
			read_latencies = new sim_time_type[2];
			read_latencies[0] = parameters->Flash_Parameters.Page_Read_Latency_LSB;
			read_latencies[1] = parameters->Flash_Parameters.Page_Read_Latency_MSB;
			write_latencies = new sim_time_type[2];
			write_latencies[0] = parameters->Flash_Parameters.Page_Program_Latency_LSB;
			write_latencies[1] = parameters->Flash_Parameters.Page_Program_Latency_MSB;
			average_flash_read_latency = (read_latencies[0] + read_latencies[1]) / 2;
			average_flash_write_latency = (write_latencies[0] + write_latencies[1]) / 2;
			break;
		case Flash_Technology_Type::TLC:
			read_latencies = new sim_time_type[3];
			read_latencies[0] = parameters->Flash_Parameters.Page_Read_Latency_LSB;
			read_latencies[1] = parameters->Flash_Parameters.Page_Read_Latency_CSB;
			read_latencies[2] = parameters->Flash_Parameters.Page_Read_Latency_MSB;
			write_latencies = new sim_time_type[3];
			write_latencies[0] = parameters->Flash_Parameters.Page_Program_Latency_LSB;
			write_latencies[1] = parameters->Flash_Parameters.Page_Program_Latency_CSB;
			write_latencies[2] = parameters->Flash_Parameters.Page_Program_Latency_MSB;
			average_flash_read_latency = (read_latencies[0] + read_latencies[1] + read_latencies[2]) / 3;
			average_flash_write_latency = (write_latencies[0] + write_latencies[1] + write_latencies[2]) / 3;
			break;
		default:
			throw std::invalid_argument("The specified flash technologies is not supported");
		}

		//Step 2: create memory channels to connect chips to the controller
		this->Channel_count = parameters->Flash_Channel_Count;
		this->Chip_no_per_channel = parameters->Chip_No_Per_Channel;
		switch (parameters->Flash_Comm_Protocol)
		{
		case SSD_Components::ONFI_Protocol::NVDDR2:
		{
			SSD_Components::ONFI_Channel_NVDDR2 **channels = new SSD_Components::ONFI_Channel_NVDDR2 *[parameters->Flash_Channel_Count];
			for (unsigned int channel_cntr = 0; channel_cntr < parameters->Flash_Channel_Count; channel_cntr++)
			{
				NVM::FlashMemory::Flash_Chip **chips = new NVM::FlashMemory::Flash_Chip *[parameters->Chip_No_Per_Channel];
				for (unsigned int chip_cntr = 0; chip_cntr < parameters->Chip_No_Per_Channel; chip_cntr++)
				{
					chips[chip_cntr] = new NVM::FlashMemory::Flash_Chip(device->ID() + ".Channel." + std::to_string(channel_cntr) + ".Chip." + std::to_string(chip_cntr),
																		channel_cntr, chip_cntr, parameters->Flash_Parameters.Flash_Technology, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
																		parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
																		read_latencies, write_latencies, parameters->Flash_Parameters.Block_Erase_Latency,
																		parameters->Flash_Parameters.Suspend_Program_Time, parameters->Flash_Parameters.Suspend_Erase_Time);
					Simulator->AddObject(chips[chip_cntr]); //Each simulation object (a child of MQSimEngine::Sim_Object) should be added to the engine
				}
				channels[channel_cntr] = new SSD_Components::ONFI_Channel_NVDDR2(channel_cntr, parameters->Chip_No_Per_Channel,
																				 chips, parameters->Flash_Channel_Width,
																				 (sim_time_type)((double)1000 / parameters->Channel_Transfer_Rate) * 2, (sim_time_type)((double)1000 / parameters->Channel_Transfer_Rate) * 2);
				device->Channels.push_back(channels[channel_cntr]); //Channels should not be added to the simulator core, they are passive object that do not handle any simulation event
			}

			//Step 3: create channel controller and connect channels to it
			device->PHY = new SSD_Components::NVM_PHY_ONFI_NVDDR2(device->ID() + ".PHY", channels, parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
																  parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die);
			Simulator->AddObject(device->PHY);
			break;
		}
		default:
			throw std::invalid_argument("No implementation is available for the specified flash communication protocol");
		}
		delete[] read_latencies;
		delete[] write_latencies;

		//Steps 4 - 8: create FTL components and connect them together
		SSD_Components::FTL *ftl = new SSD_Components::FTL(device->ID() + ".FTL", NULL, parameters->Flash_Channel_Count,
														   parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
														   parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
														   parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, average_flash_read_latency, average_flash_write_latency, parameters->Overprovisioning_Ratio,
														   parameters->Flash_Parameters.Block_PE_Cycles_Limit, parameters->Seed++);
		ftl->PHY = (SSD_Components::NVM_PHY_ONFI *)PHY;
		Simulator->AddObject(ftl);
		device->Firmware = ftl;

		//Step 5: create TSU
		SSD_Components::TSU_Base *tsu;
		bool erase_suspension = false, program_suspension = false;
		if (parameters->Flash_Parameters.CMD_Suspension_Support == NVM::FlashMemory::Command_Suspension_Mode::PROGRAM)
		{
			program_suspension = true;
		}
		if (parameters->Flash_Parameters.CMD_Suspension_Support == NVM::FlashMemory::Command_Suspension_Mode::ERASE)
		{
			erase_suspension = true;
		}
		if (parameters->Flash_Parameters.CMD_Suspension_Support == NVM::FlashMemory::Command_Suspension_Mode::PROGRAM_ERASE)
		{
			program_suspension = true;
			erase_suspension = true;
		}
		switch (parameters->Transaction_Scheduling_Policy)
		{
		case SSD_Components::Flash_Scheduling_Type::OUT_OF_ORDER:
			tsu = new SSD_Components::TSU_OutOfOrder(ftl->ID() + ".TSU", ftl, static_cast<SSD_Components::NVM_PHY_ONFI_NVDDR2 *>(device->PHY),
													 parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
													 parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
													 parameters->Preferred_suspend_write_time_for_read, parameters->Preferred_suspend_erase_time_for_read,
													 parameters->Preferred_suspend_erase_time_for_write,
													 erase_suspension, program_suspension);
			break;
		case SSD_Components::Flash_Scheduling_Type::PRIORITY_OUT_OF_ORDER:
			tsu = new SSD_Components::TSU_Priority_OutOfOrder(ftl->ID() + ".TSU", ftl, static_cast<SSD_Components::NVM_PHY_ONFI_NVDDR2 *>(device->PHY),
										  parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
										  parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
										  parameters->Preferred_suspend_write_time_for_read, parameters->Preferred_suspend_erase_time_for_read,
										  parameters->Preferred_suspend_erase_time_for_write,
										  erase_suspension, program_suspension);
			break;
		/*case SSD_Components::Flash_Scheduling_Type::FLIN:
				{
					unsigned int * stream_count_per_priority_class = new unsigned int[4];
					for (int i = 0; i < 4; i++)
						stream_count_per_priority_class[i] = 0;
					for (auto &flow : (*io_flows))
						stream_count_per_priority_class[(int)flow->Priority_Class]++;
					stream_id_type** stream_ids_per_priority_class = new stream_id_type*[4];
					for (int i = 0; i < 4; i++)
						stream_ids_per_priority_class[i] = new stream_id_type[stream_count_per_priority_class[i]];

					tsu = new SSD_Components::TSU_FLIN(ftl->ID() + ".TSU", ftl, static_cast<SSD_Components::NVM_PHY_ONFI_NVDDR2*>(device->PHY),
						parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
						parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die, parameters->Flash_Parameters.Page_Capacity,
						10000000, 33554432, 262144, 4, (unsigned int)io_flows->size(), stream_count_per_priority_class, stream_ids_per_priority_class,
						0.6,
						parameters->Preferred_suspend_write_time_for_read, parameters->Preferred_suspend_erase_time_for_read, parameters->Preferred_suspend_erase_time_for_write,
						erase_suspension, program_suspension);
					break;
				}*/
		default:
			throw std::invalid_argument("No implementation is available for the specified transaction scheduling algorithm");
		}
		Simulator->AddObject(tsu);
		ftl->TSU = tsu;

		//Step 6: create Flash_Block_Manager
		SSD_Components::Flash_Block_Manager_Base *fbm;
		fbm = new SSD_Components::Flash_Block_Manager(NULL, parameters->Flash_Parameters.Block_PE_Cycles_Limit,
													  (unsigned int)io_flows->size(), parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
													  parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
													  parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block);
		ftl->BlockManager = fbm;

		//Step 7: create Address_Mapping_Unit
		SSD_Components::Address_Mapping_Unit_Base *amu;
		std::vector<std::vector<flash_channel_ID_type>> flow_channel_id_assignments;
		std::vector<std::vector<flash_chip_ID_type>> flow_chip_id_assignments;
		std::vector<std::vector<flash_die_ID_type>> flow_die_id_assignments;
		std::vector<std::vector<flash_plane_ID_type>> flow_plane_id_assignments;
		unsigned int stream_count = 0;
		for (unsigned int i = 0; i < io_flows->size(); i++)
		{
			switch (parameters->HostInterface_Type)
			{
			case HostInterface_Types::SATA:
			{
				stream_count = 1;
				std::vector<flash_channel_ID_type> channel_ids;
				flow_channel_id_assignments.push_back(channel_ids);
				for (unsigned int j = 0; j < parameters->Flash_Channel_Count; j++)
				{
					flow_channel_id_assignments[i].push_back(j);
				}
				std::vector<flash_chip_ID_type> chip_ids;
				flow_chip_id_assignments.push_back(chip_ids);
				for (unsigned int j = 0; j < parameters->Chip_No_Per_Channel; j++)
				{
					flow_chip_id_assignments[i].push_back(j);
				}
				std::vector<flash_die_ID_type> die_ids;
				flow_die_id_assignments.push_back(die_ids);
				for (unsigned int j = 0; j < parameters->Flash_Parameters.Die_No_Per_Chip; j++)
				{
					flow_die_id_assignments[i].push_back(j);
				}
				std::vector<flash_plane_ID_type> plane_ids;
				flow_plane_id_assignments.push_back(plane_ids);
				for (unsigned int j = 0; j < parameters->Flash_Parameters.Plane_No_Per_Die; j++)
				{
					flow_plane_id_assignments[i].push_back(j);
				}
				break;
			}
			case HostInterface_Types::NVME:
			{
				stream_count = (unsigned int)io_flows->size();
				std::vector<flash_channel_ID_type> channel_ids;
				flow_channel_id_assignments.push_back(channel_ids);
				for (int j = 0; j < (*io_flows)[i]->Channel_No; j++)
				{
					flow_channel_id_assignments[i].push_back((*io_flows)[i]->Channel_IDs[j]);
				}
				std::vector<flash_chip_ID_type> chip_ids;
				flow_chip_id_assignments.push_back(chip_ids);
				for (int j = 0; j < (*io_flows)[i]->Chip_No; j++)
				{
					flow_chip_id_assignments[i].push_back((*io_flows)[i]->Chip_IDs[j]);
				}
				std::vector<flash_die_ID_type> die_ids;
				flow_die_id_assignments.push_back(die_ids);
				for (int j = 0; j < (*io_flows)[i]->Die_No; j++)
				{
					flow_die_id_assignments[i].push_back((*io_flows)[i]->Die_IDs[j]);
				}
				std::vector<flash_plane_ID_type> plane_ids;
				flow_plane_id_assignments.push_back(plane_ids);
				for (int j = 0; j < (*io_flows)[i]->Plane_No; j++)
				{
					flow_plane_id_assignments[i].push_back((*io_flows)[i]->Plane_IDs[j]);
				}
				break;
			}
			default:
				break;
			}
		}

		Utils::Logical_Address_Partitioning_Unit::Allocate_logical_address_for_flows(parameters->HostInterface_Type, (unsigned int)io_flows->size(),
																					 parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
																					 flow_channel_id_assignments, flow_chip_id_assignments, flow_die_id_assignments, flow_plane_id_assignments,
																					 parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
																					 parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Overprovisioning_Ratio);
		switch (parameters->Address_Mapping)
		{
		case SSD_Components::Flash_Address_Mapping_Type::PAGE_LEVEL:
			amu = new SSD_Components::Address_Mapping_Unit_Page_Level(ftl->ID() + ".AddressMappingUnit", ftl, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
																	  fbm, parameters->Ideal_Mapping_Table, parameters->CMT_Capacity, parameters->Plane_Allocation_Scheme, stream_count,
																	  parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
																	  flow_channel_id_assignments, flow_chip_id_assignments, flow_die_id_assignments, flow_plane_id_assignments,
																	  parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
																	  parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Flash_Parameters.Page_Capacity, parameters->Overprovisioning_Ratio,
																	  parameters->CMT_Sharing_Mode);
			break;
		case SSD_Components::Flash_Address_Mapping_Type::HYBRID:
			amu = new SSD_Components::Address_Mapping_Unit_Hybrid(ftl->ID() + ".AddressMappingUnit", ftl, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
																  fbm, parameters->Ideal_Mapping_Table, stream_count,
																  parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip,
																  parameters->Flash_Parameters.Plane_No_Per_Die, parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
																  parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Flash_Parameters.Page_Capacity, parameters->Overprovisioning_Ratio);
			break;
		default:
			throw std::invalid_argument("No implementation is available fo the secified address mapping strategy");
		}
		Simulator->AddObject(amu);
		ftl->Address_Mapping_Unit = amu;

		//Step 8: create GC_and_WL_unit
		double max_rho = 0;
		for (unsigned int i = 0; i < io_flows->size(); i++)
		{
			if ((*io_flows)[i]->Initial_Occupancy_Percentage > max_rho)
			{
				max_rho = (*io_flows)[i]->Initial_Occupancy_Percentage;
			}
		}
		max_rho /= 100; //Convert from percentage to a value between zero and 1
		SSD_Components::GC_and_WL_Unit_Base *gcwl;
		gcwl = new SSD_Components::GC_and_WL_Unit_Page_Level(ftl->ID() + ".GCandWLUnit", amu, fbm, tsu, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
															 parameters->GC_Block_Selection_Policy, parameters->GC_Exec_Threshold, parameters->Preemptible_GC_Enabled, parameters->GC_Hard_Threshold,
															 parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
															 parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
															 parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
															 parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Use_Copyback_for_GC, max_rho, 10,
															 parameters->Seed++);
		Simulator->AddObject(gcwl);
		fbm->Set_GC_and_WL_Unit(gcwl);
		ftl->GC_and_WL_Unit = gcwl;

		//Step 9: create Data_Cache_Manager
		SSD_Components::Data_Cache_Manager_Base *dcm;
		SSD_Components::Caching_Mode *caching_modes = new SSD_Components::Caching_Mode[io_flows->size()];
		for (unsigned int i = 0; i < io_flows->size(); i++)
		{
			caching_modes[i] = (*io_flows)[i]->Device_Level_Data_Caching_Mode;
		}

		switch (parameters->Caching_Mechanism)
		{
		case SSD_Components::Caching_Mechanism::SIMPLE:
			dcm = new SSD_Components::Data_Cache_Manager_Flash_Simple(device->ID() + ".DataCache", NULL, ftl, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
																	  parameters->Data_Cache_Capacity, parameters->Data_Cache_DRAM_Row_Size, parameters->Data_Cache_DRAM_Data_Rate,
																	  parameters->Data_Cache_DRAM_Data_Busrt_Size, parameters->Data_Cache_DRAM_tRCD, parameters->Data_Cache_DRAM_tCL, parameters->Data_Cache_DRAM_tRP,
																	  caching_modes, (unsigned int)io_flows->size(),
																	  parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Flash_Channel_Count * parameters->Chip_No_Per_Channel * parameters->Flash_Parameters.Die_No_Per_Chip * parameters->Flash_Parameters.Plane_No_Per_Die * parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE);

			break;
		case SSD_Components::Caching_Mechanism::ADVANCED:
			dcm = new SSD_Components::Data_Cache_Manager_Flash_Advanced(device->ID() + ".DataCache", NULL, ftl, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
																		parameters->Data_Cache_Capacity, parameters->Data_Cache_DRAM_Row_Size, parameters->Data_Cache_DRAM_Data_Rate,
																		parameters->Data_Cache_DRAM_Data_Busrt_Size, parameters->Data_Cache_DRAM_tRCD, parameters->Data_Cache_DRAM_tCL, parameters->Data_Cache_DRAM_tRP,
																		caching_modes, parameters->Data_Cache_Sharing_Mode, (unsigned int)io_flows->size(),
																		parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Flash_Channel_Count * parameters->Chip_No_Per_Channel * parameters->Flash_Parameters.Die_No_Per_Chip * parameters->Flash_Parameters.Plane_No_Per_Die * parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE);

			break;
		default:
			PRINT_ERROR("Unknown data caching mechanism!")
		}

		Simulator->AddObject(dcm);
		ftl->Data_cache_manager = dcm;
		device->Cache_manager = dcm;

		//Step 10: create Host_Interface
		switch (parameters->HostInterface_Type)
		{
		case HostInterface_Types::NVME:
			device->Host_interface = new SSD_Components::Host_Interface_NVMe(device->ID() + ".HostInterface",
																			 Utils::Logical_Address_Partitioning_Unit::Get_total_device_lha_count(), parameters->IO_Queue_Depth, parameters->IO_Queue_Depth,
																			 (unsigned int)io_flows->size(), parameters->Queue_Fetch_Size, parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, dcm);
			break;
		case HostInterface_Types::SATA:
			device->Host_interface = new SSD_Components::Host_Interface_SATA(device->ID() + ".HostInterface",
																			 parameters->IO_Queue_Depth, Utils::Logical_Address_Partitioning_Unit::Get_total_device_lha_count(), parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, dcm);

			break;
		default:
			break;
		}
		Simulator->AddObject(device->Host_interface);
		dcm->Set_host_interface(device->Host_interface);
		break;
	}
	default:
		throw std::invalid_argument("Undefined NVM type specified ");
	} // switch (Memory_Type)
}

SSD_Device::~SSD_Device()
{
	for (unsigned int channel_cntr = 0; channel_cntr < Channel_count; channel_cntr++)
	{
		for (unsigned int chip_cntr = 0; chip_cntr < Chip_no_per_channel; chip_cntr++)
		{
			delete ((SSD_Components::ONFI_Channel_NVDDR2 *)this->Channels[channel_cntr])->Chips[chip_cntr];
		}
		delete this->Channels[channel_cntr];
	}

	delete this->PHY;
	delete ((SSD_Components::FTL *)this->Firmware)->TSU;
	delete ((SSD_Components::FTL *)this->Firmware)->BlockManager;
	delete ((SSD_Components::FTL *)this->Firmware)->Address_Mapping_Unit;
	delete ((SSD_Components::FTL *)this->Firmware)->GC_and_WL_Unit;
	delete this->Firmware;
	delete this->Cache_manager;
	delete this->Host_interface;
}

void SSD_Device::Attach_to_host(Host_Components::PCIe_Switch *pcie_switch)
{
	this->Host_interface->Attach_to_device(pcie_switch);
}

void SSD_Device::Perform_preconditioning(std::vector<Utils::Workload_Statistics *> workload_stats)
{
	if (Preconditioning_required)
	{
		time_t start_time = time(0);
		PRINT_MESSAGE("SSD Device preconditioning started .........");
		this->Firmware->Perform_precondition(workload_stats);
		this->Cache_manager->Do_warmup(workload_stats);
		time_t end_time = time(0);
		uint64_t duration = (uint64_t)difftime(end_time, start_time);
		PRINT_MESSAGE("Finished preconditioning. Duration of preconditioning: " << duration / 3600 << ":" << (duration % 3600) / 60 << ":" << ((duration % 3600) % 60));
	}
}

void SSD_Device::Start_simulation()
{
}

void SSD_Device::Validate_simulation_config()
{
}

void SSD_Device::Execute_simulator_event(MQSimEngine::Sim_Event *event)
{
}

void SSD_Device::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter &xmlwriter)
{
	std::string tmp = ID();
	xmlwriter.Write_open_tag(tmp);

	this->Host_interface->Report_results_in_XML(ID(), xmlwriter);
	if (Memory_Type == NVM::NVM_Type::FLASH)
	{
		SSD_Components::FTL* ftl = static_cast<SSD_Components::FTL*>(this->Firmware);
		ftl->Report_results_in_XML(ID(), xmlwriter);
		ftl->TSU->Report_results_in_XML(ID(), xmlwriter);

		unsigned long long total_flash_page_programs = 0;
		unsigned long long total_flash_page_erases = 0;
		Erase_Distribution_Summary ssd_summary;
		std::map<unsigned int, uint64_t> ssd_erase_histogram;
		unsigned int plane_count = 0;
		unsigned int worst_plane_spread = 0;
		double sum_plane_spread = 0.0;

		if (ftl != nullptr && ftl->BlockManager != nullptr) {
			for (unsigned int ch = 0; ch < Channel_count; ch++) {
				SSD_Components::ONFI_Channel_Base* channel = static_cast<SSD_Components::ONFI_Channel_Base*>(Channels[ch]);
				for (unsigned int chip = 0; chip < Chip_no_per_channel; chip++) {
					NVM::FlashMemory::Flash_Chip* flash_chip = channel->Chips[chip];
					if (flash_chip != nullptr) {
						total_flash_page_programs += flash_chip->Get_total_program_count();
						total_flash_page_erases += flash_chip->Get_total_erase_count();
					}
					for (unsigned int die = 0; die < Die_no_per_chip; die++) {
						for (unsigned int plane = 0; plane < Plane_no_per_die; plane++) {
							NVM::FlashMemory::Physical_Page_Address plane_address(ch, chip, die, plane, 0, 0);
							SSD_Components::PlaneBookKeepingType* pbke = ftl->BlockManager->Get_plane_bookkeeping_entry(plane_address);
							if (pbke == nullptr || pbke->Blocks == nullptr) {
								continue;
							}

							Erase_Distribution_Summary plane_summary;
							for (unsigned int block = 0; block < Block_no_per_plane; block++) {
								unsigned int erase_count = pbke->Blocks[block].Erase_count;
								update_erase_summary(plane_summary, erase_count);
								update_erase_summary(ssd_summary, erase_count);
								ssd_erase_histogram[erase_count]++;
							}

							unsigned int plane_spread = plane_summary.Block_count == 0 ? 0 : (plane_summary.Max_erase_count - erase_min_or_zero(plane_summary));
							if (plane_spread > worst_plane_spread) {
								worst_plane_spread = plane_spread;
							}
							sum_plane_spread += plane_spread;
							plane_count++;

							std::string plane_tag = ID() + ".WearLeveling.Plane";
							xmlwriter.Write_open_tag(plane_tag);
							xmlwriter.Write_attribute_string("SSD_ID", "0");
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

		unsigned long long host_write_bytes = (unsigned long long)External_host_write_bytes;
		unsigned long long flash_programmed_bytes = total_flash_page_programs * (unsigned long long)Page_capacity_bytes;
		double approx_flash_wa = host_write_bytes == 0 ? 0.0 :
			(double)flash_programmed_bytes / (double)host_write_bytes;

		std::string wl_tag = ID() + ".WearLeveling";
		xmlwriter.Write_open_tag(wl_tag);

		std::string ssd_tag = wl_tag + ".SSD";
		xmlwriter.Write_open_tag(ssd_tag);
		xmlwriter.Write_attribute_string("SSD_ID", "0");
		xmlwriter.Write_attribute_string("Plane_Count", std::to_string(plane_count));
		xmlwriter.Write_attribute_string("Block_Count", std::to_string(ssd_summary.Block_count));
		xmlwriter.Write_attribute_string("Min_Block_Erase_Count", std::to_string(erase_min_or_zero(ssd_summary)));
		xmlwriter.Write_attribute_string("Max_Block_Erase_Count", std::to_string(ssd_summary.Max_erase_count));
		xmlwriter.Write_attribute_string("Avg_Block_Erase_Count", std::to_string(erase_avg(ssd_summary)));
		xmlwriter.Write_attribute_string("StdDev_Block_Erase_Count", std::to_string(erase_stddev(ssd_summary)));
		xmlwriter.Write_attribute_string("Erase_Count_Spread", std::to_string(ssd_summary.Block_count == 0 ? 0 : (ssd_summary.Max_erase_count - erase_min_or_zero(ssd_summary))));
		xmlwriter.Write_attribute_string("Worst_Plane_Erase_Spread", std::to_string(worst_plane_spread));
		xmlwriter.Write_attribute_string("Average_Plane_Erase_Spread", std::to_string(plane_count == 0 ? 0.0 : sum_plane_spread / (double)plane_count));
		xmlwriter.Write_attribute_string("Flash_Page_Program_Count", std::to_string(total_flash_page_programs));
		xmlwriter.Write_attribute_string("Flash_Page_Erase_Count", std::to_string(total_flash_page_erases));
		xmlwriter.Write_attribute_string("Host_Write_Bytes", std::to_string(host_write_bytes));
		xmlwriter.Write_attribute_string("Approx_Flash_Programmed_Bytes", std::to_string(flash_programmed_bytes));
		xmlwriter.Write_attribute_string("Approx_Flash_Write_Amplification", std::to_string(approx_flash_wa));
		xmlwriter.Write_close_tag();

		std::string hist_tag = wl_tag + ".EraseHistogram.SSD";
		xmlwriter.Write_open_tag(hist_tag);
		xmlwriter.Write_attribute_string("SSD_ID", "0");
		xmlwriter.Write_attribute_string("Block_Count", std::to_string(ssd_summary.Block_count));
		for (const auto& bin : ssd_erase_histogram) {
			std::string bin_tag = hist_tag + ".Bin";
			xmlwriter.Write_open_tag(bin_tag);
			xmlwriter.Write_attribute_string("Erase_Count", std::to_string(bin.first));
			xmlwriter.Write_attribute_string("Block_Count", std::to_string(bin.second));
			xmlwriter.Write_attribute_string("Fraction", std::to_string(ssd_summary.Block_count == 0 ? 0.0 : (double)bin.second / (double)ssd_summary.Block_count));
			xmlwriter.Write_close_tag();
		}
		xmlwriter.Write_close_tag();

		std::string wa_tag = wl_tag + ".WriteAmplification";
		xmlwriter.Write_open_tag(wa_tag);
		xmlwriter.Write_attribute_string("Host_Write_Bytes", std::to_string(host_write_bytes));
		xmlwriter.Write_attribute_string("Approx_Flash_Page_Programs", std::to_string(total_flash_page_programs));
		xmlwriter.Write_attribute_string("Approx_Flash_Page_Erases", std::to_string(total_flash_page_erases));
		xmlwriter.Write_attribute_string("Approx_Flash_Programmed_Bytes", std::to_string(flash_programmed_bytes));
		xmlwriter.Write_attribute_string("Approx_Flash_Write_Amplification", std::to_string(approx_flash_wa));
		xmlwriter.Write_close_tag();

		xmlwriter.Write_close_tag();

		for (unsigned int channel_cntr = 0; channel_cntr < Channel_count; channel_cntr++)
		{
			for (unsigned int chip_cntr = 0; chip_cntr < Chip_no_per_channel; chip_cntr++)
			{
				((SSD_Components::ONFI_Channel_NVDDR2 *)Channels[channel_cntr])->Chips[chip_cntr]->Report_results_in_XML(ID(), xmlwriter);
			}
		}
	}
	xmlwriter.Write_close_tag();
}

unsigned int SSD_Device::Get_no_of_LHAs_in_an_NVM_write_unit()
{
	return Host_interface->Get_no_of_LHAs_in_an_NVM_write_unit();
}

LPA_type SSD_Device::Convert_host_logical_address_to_device_address(LHA_type lha)
{
	return my_instance->Firmware->Convert_host_logical_address_to_device_address(lha);
}

page_status_type SSD_Device::Find_NVM_subunit_access_bitmap(LHA_type lha)
{
	return my_instance->Firmware->Find_NVM_subunit_access_bitmap(lha);
}
