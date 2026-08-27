#include "Device_Parameter_Set.h"
#include <algorithm>
Device_Parameter_Set::Device_Parameter_Set()
	: Seed(123),
	  Enabled_Preconditioning(true),
	  Memory_Type(NVM::NVM_Type::FLASH),
	  HostInterface_Type(HostInterface_Types::NVME),
	  IO_Queue_Depth(1024),
	  Queue_Fetch_Size(512),
	  Caching_Mechanism(SSD_Components::Caching_Mechanism::ADVANCED),
	  Data_Cache_Sharing_Mode(SSD_Components::Cache_Sharing_Mode::SHARED),
	  Data_Cache_Capacity(1024 * 1024 * 512),
	  Data_Cache_DRAM_Row_Size(8192),
	  Data_Cache_DRAM_Data_Rate(800),
	  Data_Cache_DRAM_Data_Busrt_Size(4),
	  Data_Cache_DRAM_tRCD(13),
	  Data_Cache_DRAM_tCL(13),
	  Data_Cache_DRAM_tRP(13),
	  Address_Mapping(SSD_Components::Flash_Address_Mapping_Type::PAGE_LEVEL),
	  Ideal_Mapping_Table(false),
	  CMT_Capacity(2 * 1024 * 1024),
	  CMT_Sharing_Mode(SSD_Components::CMT_Sharing_Mode::SHARED),
	  Plane_Allocation_Scheme(SSD_Components::Flash_Plane_Allocation_Scheme_Type::CWDP),
	  Transaction_Scheduling_Policy(SSD_Components::Flash_Scheduling_Type::OUT_OF_ORDER),
	  Overprovisioning_Ratio(0.07),
	  GC_Exec_Threshold(0.05),
	  GC_Block_Selection_Policy(SSD_Components::GC_Block_Selection_Policy_Type::RGA),
	  Use_Copyback_for_GC(false),
	  Preemptible_GC_Enabled(true),
	  GC_Hard_Threshold(0.005),
	  Dynamic_Wearleveling_Enabled(true),
	  Static_Wearleveling_Enabled(true),
	  Static_Wearleveling_Threshold(100),
	  Preferred_suspend_erase_time_for_read(700000),
	  Preferred_suspend_erase_time_for_write(700000),
	  Preferred_suspend_write_time_for_read(100000),
	  Flash_Channel_Count(8),
	  Flash_Channel_Width(1),
	  Channel_Transfer_Rate(300),
	  Chip_No_Per_Channel(4),
	  Flash_Comm_Protocol(SSD_Components::ONFI_Protocol::NVDDR2),
	  SSD_Count(1),
	  Stripe_Unit_LBA(8),
	  SWANS_Enabled(false),
	  SWANS_Zone_Size_LBA(0),
	  Zone_Stripe_Multiplier(128),
	  SWANS_Epoch_Length(40000000000ULL),
	  SWANS_Epoch_Default(40000000000ULL),
	  SWANS_Epoch_Placement(40000000000ULL),
	  SWANS_Epoch_Migration(40000000000ULL),
	  SWANS_TH_Precautionary(5.0 * 1024.0 * 1024.0),
	  SWANS_TH_Critical(15.0 * 1024.0 * 1024.0),
	  SWANS_Max_Concurrent_Migrations(1),
	  SWANS_Migration_Buffer_Limit(64),
	  SWANS_Migration_Working_Queue_Limit(64),
	  SWANS_Buffered_Write_Completion_Mode("DEFERRED"),
	  Bad_Block_Retirement_Enabled(false),
	  Simulation_Stop_Mode("FIXED"),
	  End_of_Life_Threshold(0.05)
{
}
// static -> 파라미터 멤버화

void Device_Parameter_Set::XML_serialize(Utils::XmlWriter& xmlwriter)
{
	std::string tmp;
	tmp = "Device_Parameter_Set";
	xmlwriter.Write_open_tag(tmp);

	std::string attr = "Seed";
	std::string val = std::to_string(Seed);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Enabled_Preconditioning";
	val = (Enabled_Preconditioning ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Memory_Type";
	val;
	switch (Memory_Type) {
		case NVM::NVM_Type::FLASH:
			val = "FLASH";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "HostInterface_Type";
	val;
	switch (HostInterface_Type) {
		case HostInterface_Types::NVME:
			val = "NVME";
			break;
		case HostInterface_Types::SATA:
			val = "SATA";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "IO_Queue_Depth";
	val = std::to_string(IO_Queue_Depth);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Queue_Fetch_Size";
	val = std::to_string(Queue_Fetch_Size);
	xmlwriter.Write_attribute_string(attr, val);


	attr = "Caching_Mechanism";
	switch (Caching_Mechanism) {
		case SSD_Components::Caching_Mechanism::SIMPLE:
			val = "SIMPLE";
			break;
		case SSD_Components::Caching_Mechanism::ADVANCED:
			val = "ADVANCED";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_Sharing_Mode";
	switch (Data_Cache_Sharing_Mode) {
		case SSD_Components::Cache_Sharing_Mode::SHARED:
			val = "SHARED";
			break;
		case SSD_Components::Cache_Sharing_Mode::EQUAL_PARTITIONING:
			val = "EQUAL_PARTITIONING";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_Capacity";
	val = std::to_string(Data_Cache_Capacity);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_DRAM_Row_Size";
	val = std::to_string(Data_Cache_DRAM_Row_Size);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_DRAM_Data_Rate";
	val = std::to_string(Data_Cache_DRAM_Data_Rate);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_DRAM_Data_Busrt_Size";
	val = std::to_string(Data_Cache_DRAM_Data_Busrt_Size);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_DRAM_tRCD";
	val = std::to_string(Data_Cache_DRAM_tRCD);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_DRAM_tCL";
	val = std::to_string(Data_Cache_DRAM_tCL);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_DRAM_tRP";
	val = std::to_string(Data_Cache_DRAM_tRP);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Address_Mapping";
	switch (Address_Mapping) {
		case SSD_Components::Flash_Address_Mapping_Type::PAGE_LEVEL:
			val = "PAGE_LEVEL";
			break;
		case SSD_Components::Flash_Address_Mapping_Type::HYBRID:
			val = "HYBRID";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Ideal_Mapping_Table";
	val = (Ideal_Mapping_Table ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);
	
	attr = "CMT_Capacity";
	val = std::to_string(CMT_Capacity);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "CMT_Sharing_Mode";
	switch (CMT_Sharing_Mode) {
		case SSD_Components::CMT_Sharing_Mode::SHARED:
			val = "SHARED";
			break;
		case SSD_Components::CMT_Sharing_Mode::EQUAL_SIZE_PARTITIONING:
			val = "EQUAL_SIZE_PARTITIONING";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Plane_Allocation_Scheme";
	switch (Plane_Allocation_Scheme) {
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::CDPW:
			val = "CDPW";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::CDWP:
			val = "CDWP";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::CPDW:
			val = "CPDW";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::CPWD:
			val = "CPWD";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::CWDP:
			val = "CWDP";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::CWPD:
			val = "CWPD";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::DCPW:
			val = "DCPW";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::DCWP:
			val = "DCWP";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::DPCW:
			val = "DPCW";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::DPWC:
			val = "DPWC";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::DWCP:
			val = "DWCP";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::DWPC:
			val = "DWPC";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::PCDW:
			val = "PCDW";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::PCWD:
			val = "PCWD";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::PDCW:
			val = "PDCW";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::PDWC:
			val = "PDWC";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::PWCD:
			val = "PWCD";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::PWDC:
			val = "PWDC";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::WCDP:
			val = "WCDP";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::WCPD:
			val = "WCPD";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::WDCP:
			val = "WDCP";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::WDPC:
			val = "WDPC";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::WPCD:
			val = "WPCD";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::WPDC:
			val = "WPDC";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Transaction_Scheduling_Policy";
	switch (Transaction_Scheduling_Policy) {
		case SSD_Components::Flash_Scheduling_Type::OUT_OF_ORDER:
			val = "OUT_OF_ORDER";
			break;
		case SSD_Components::Flash_Scheduling_Type::PRIORITY_OUT_OF_ORDER:
			val = "PRIORITY_OUT_OF_ORDER";
			break;
		case SSD_Components::Flash_Scheduling_Type::FLIN:
			val = "FLIN";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Overprovisioning_Ratio";
	val = std::to_string(Overprovisioning_Ratio);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "GC_Exec_Threshold";
	val = std::to_string(GC_Exec_Threshold);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "GC_Block_Selection_Policy";
	switch (GC_Block_Selection_Policy) {
		case SSD_Components::GC_Block_Selection_Policy_Type::GREEDY:
			val = "GREEDY";
			break;
		case SSD_Components::GC_Block_Selection_Policy_Type::RGA:
			val = "RGA";
			break;
		case SSD_Components::GC_Block_Selection_Policy_Type::RANDOM:
			val = "RANDOM";
			break;
		case SSD_Components::GC_Block_Selection_Policy_Type::RANDOM_P:
			val = "RANDOM_P";
			break;
		case SSD_Components::GC_Block_Selection_Policy_Type::RANDOM_PP:
			val = "RANDOM_PP";
			break;
		case SSD_Components::GC_Block_Selection_Policy_Type::FIFO:
			val = "FIFO";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);
	
	attr = "Use_Copyback_for_GC";
	val = (Use_Copyback_for_GC ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Preemptible_GC_Enabled";
	val = (Preemptible_GC_Enabled ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);

	attr = "GC_Hard_Threshold";
	val = std::to_string(GC_Hard_Threshold);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Dynamic_Wearleveling_Enabled";
	val = (Dynamic_Wearleveling_Enabled ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Static_Wearleveling_Enabled";
	val = (Static_Wearleveling_Enabled ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);
	
	attr = "Static_Wearleveling_Threshold";
	val = std::to_string(Static_Wearleveling_Threshold);
	xmlwriter.Write_attribute_string(attr, val);
	
	attr = "Preferred_suspend_erase_time_for_read";
	val = std::to_string(Preferred_suspend_erase_time_for_read);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Preferred_suspend_erase_time_for_write";
	val = std::to_string(Preferred_suspend_erase_time_for_write);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Preferred_suspend_write_time_for_read";
	val = std::to_string(Preferred_suspend_write_time_for_read);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Flash_Channel_Count";
	val = std::to_string(Flash_Channel_Count);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Flash_Channel_Width";
	val = std::to_string(Flash_Channel_Width);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Channel_Transfer_Rate";
	val = std::to_string(Channel_Transfer_Rate);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Chip_No_Per_Channel";
	val = std::to_string(Chip_No_Per_Channel);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Flash_Comm_Protocol";
	switch (Flash_Comm_Protocol) {
		case SSD_Components::ONFI_Protocol::NVDDR2:
			val = "NVDDR2";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	// RAID 파라미터 추가
	attr = "SSD_Count";
	val = std::to_string(SSD_Count);
	xmlwriter.Write_attribute_string(attr, val);     // SSD 개수

	attr = "Stripe_Unit_LBA";
	val = std::to_string(Stripe_Unit_LBA);
	xmlwriter.Write_attribute_string(attr, val);     // 스트라이프 유닛 크기

	attr = "SWANS_Enabled";
	val = (SWANS_Enabled ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);

	attr = "SWANS_Zone_Size_LBA";
	val = std::to_string(SWANS_Zone_Size_LBA);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Zone_Stripe_Multiplier";
	val = std::to_string(Zone_Stripe_Multiplier);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "SWANS_Epoch_Length";
	val = std::to_string(SWANS_Epoch_Length);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "SWANS_Epoch_Default";
	val = std::to_string(SWANS_Epoch_Default);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "SWANS_Epoch_Placement";
	val = std::to_string(SWANS_Epoch_Placement);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "SWANS_Epoch_Migration";
	val = std::to_string(SWANS_Epoch_Migration);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "SWANS_TH_Precautionary";
	val = std::to_string(SWANS_TH_Precautionary);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "SWANS_TH_Critical";
	val = std::to_string(SWANS_TH_Critical);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "SWANS_Max_Concurrent_Migrations";
	val = std::to_string(SWANS_Max_Concurrent_Migrations);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "SWANS_Migration_Buffer_Limit";
	val = std::to_string(SWANS_Migration_Buffer_Limit);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "SWANS_Migration_Working_Queue_Limit";
	val = std::to_string(SWANS_Migration_Working_Queue_Limit);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "SWANS_Buffered_Write_Completion_Mode";
	val = SWANS_Buffered_Write_Completion_Mode;
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Bad_Block_Retirement_Enabled";
	val = (Bad_Block_Retirement_Enabled ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Simulation_Stop_Mode";
	val = Simulation_Stop_Mode;
	xmlwriter.Write_attribute_string(attr, val);

	attr = "End_of_Life_Threshold";
	val = std::to_string(End_of_Life_Threshold);
	xmlwriter.Write_attribute_string(attr, val);

	Flash_Parameters.XML_serialize(xmlwriter);

	xmlwriter.Write_close_tag();
}

void Device_Parameter_Set::XML_deserialize(rapidxml::xml_node<> *node)
{
	try
	{
		for (auto param = node->first_node(); param; param = param->next_sibling()) {
			if (strcmp(param->name(), "Seed") == 0) {
				std::string val = param->value();
				Seed = std::stoi(val);
			} else if (strcmp(param->name(), "Enabled_Preconditioning") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				Enabled_Preconditioning = (val.compare("FALSE") == 0 ? false : true);
			} else if (strcmp(param->name(), "Memory_Type") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "FLASH") == 0)
					Memory_Type = NVM::NVM_Type::FLASH;
				else PRINT_ERROR("Unknown NVM type specified in the SSD configuration file")
			} else if (strcmp(param->name(), "HostInterface_Type") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "NVME") == 0) {
					HostInterface_Type = HostInterface_Types::NVME;
				} else if (strcmp(val.c_str(), "SATA") == 0) {
					HostInterface_Type = HostInterface_Types::SATA;
				} else {
					PRINT_ERROR("Unknown host interface type specified in the SSD configuration file")
				}
			} else if (strcmp(param->name(), "IO_Queue_Depth") == 0) {
				std::string val = param->value();
				IO_Queue_Depth = (uint16_t) std::stoull(val);
			} else if (strcmp(param->name(), "Queue_Fetch_Size") == 0) {
				std::string val = param->value();
				Queue_Fetch_Size = (uint16_t) std::stoull(val);
			} else if (strcmp(param->name(), "Caching_Mechanism") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "SIMPLE") == 0) {
					Caching_Mechanism = SSD_Components::Caching_Mechanism::SIMPLE;
				} else if (strcmp(val.c_str(), "ADVANCED") == 0) {
					Caching_Mechanism = SSD_Components::Caching_Mechanism::ADVANCED;
				} else {
					PRINT_ERROR("Unknown data caching mechanism specified in the SSD configuration file")
				}
			} else if (strcmp(param->name(), "Data_Cache_Sharing_Mode") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "SHARED") == 0) {
					Data_Cache_Sharing_Mode = SSD_Components::Cache_Sharing_Mode::SHARED;
				} else if (strcmp(val.c_str(), "EQUAL_PARTITIONING") == 0) {
					Data_Cache_Sharing_Mode = SSD_Components::Cache_Sharing_Mode::EQUAL_PARTITIONING;
				} else {
					PRINT_ERROR("Unknown data cache sharing mode specified in the SSD configuration file")
				}
			} else if (strcmp(param->name(), "Data_Cache_Capacity") == 0) {
				std::string val = param->value();
				Data_Cache_Capacity = std::stoul(val);
			} else if (strcmp(param->name(), "Data_Cache_DRAM_Row_Size") == 0) {
				std::string val = param->value();
				Data_Cache_DRAM_Row_Size = std::stoul(val);
			} else if (strcmp(param->name(), "Data_Cache_DRAM_Data_Rate") == 0) {
				std::string val = param->value();
				Data_Cache_DRAM_Data_Rate = std::stoul(val);
			} else if (strcmp(param->name(), "Data_Cache_DRAM_Data_Busrt_Size") == 0) {
				std::string val = param->value();
				Data_Cache_DRAM_Data_Busrt_Size = std::stoul(val);
			} else if (strcmp(param->name(), "Data_Cache_DRAM_tRCD") == 0) {
				std::string val = param->value();
				Data_Cache_DRAM_tRCD = std::stoul(val);
			} else if (strcmp(param->name(), "Data_Cache_DRAM_tCL") == 0) {
				std::string val = param->value();
				Data_Cache_DRAM_tCL = std::stoul(val);
			} else if (strcmp(param->name(), "Data_Cache_DRAM_tRP") == 0) {
				std::string val = param->value();
				Data_Cache_DRAM_tRP = std::stoul(val);
			} else if (strcmp(param->name(), "Address_Mapping") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "PAGE_LEVEL") == 0) {
					Address_Mapping = SSD_Components::Flash_Address_Mapping_Type::PAGE_LEVEL;
				} else if (strcmp(val.c_str(), "HYBRID") == 0) {
					Address_Mapping = SSD_Components::Flash_Address_Mapping_Type::HYBRID;
				} else {
					PRINT_ERROR("Unknown address mapping type specified in the SSD configuration file")
				}
			}
			else if (strcmp(param->name(), "Ideal_Mapping_Table") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				Ideal_Mapping_Table = (val.compare("FALSE") == 0 ? false : true);
			} else if (strcmp(param->name(), "CMT_Capacity") == 0) {
				std::string val = param->value();
				CMT_Capacity = std::stoul(val);
			} else if (strcmp(param->name(), "CMT_Sharing_Mode") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "SHARED") == 0) {
					CMT_Sharing_Mode = SSD_Components::CMT_Sharing_Mode::SHARED;
				} else if (strcmp(val.c_str(), "EQUAL_PARTITIONING") == 0) {
					CMT_Sharing_Mode = SSD_Components::CMT_Sharing_Mode::EQUAL_SIZE_PARTITIONING;
				} else {
					PRINT_ERROR("Unknown CMT sharing mode specified in the SSD configuration file")
				}
			} else if (strcmp(param->name(), "Plane_Allocation_Scheme") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "CDPW") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::CDPW;
				} else if (strcmp(val.c_str(), "CDWP") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::CDWP;
				} else if (strcmp(val.c_str(), "CPDW") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::CPDW;
				} else if (strcmp(val.c_str(), "CPWD") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::CPWD;
				} else if (strcmp(val.c_str(), "CWDP") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::CWDP;
				} else if (strcmp(val.c_str(), "CWPD") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::CWPD;
				} else if (strcmp(val.c_str(), "DCPW") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::DCPW;
				} else if (strcmp(val.c_str(), "DCWP") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::DCWP;
				} else if (strcmp(val.c_str(), "DPCW") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::DPCW;
				} else if (strcmp(val.c_str(), "DPWC") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::DPWC;
				} else if (strcmp(val.c_str(), "DWCP") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::DWCP;
				} else if (strcmp(val.c_str(), "DWPC") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::DWPC;
				} else if (strcmp(val.c_str(), "PCDW") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::PCDW;
				} else if (strcmp(val.c_str(), "PCWD") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::PCWD;
				} else if (strcmp(val.c_str(), "PDCW") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::PDCW;
				} else if (strcmp(val.c_str(), "PDWC") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::PDWC;
				} else if (strcmp(val.c_str(), "PWCD") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::PWCD;
				} else if (strcmp(val.c_str(), "PWDC") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::PWDC;
				} else if (strcmp(val.c_str(), "WCDP") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::WCDP;
				} else if (strcmp(val.c_str(), "WCPD") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::WCPD;
				} else if (strcmp(val.c_str(), "WDCP") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::WDCP;
				} else if (strcmp(val.c_str(), "WDPC") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::WDPC;
				} else if (strcmp(val.c_str(), "WPCD") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::WPCD;
				} else if (strcmp(val.c_str(), "WPDC") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::WPDC;
				} else {
					PRINT_ERROR("Unknown plane allocation scheme type specified in the SSD configuration file")
				}
			} else if (strcmp(param->name(), "Transaction_Scheduling_Policy") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "OUT_OF_ORDER") == 0) {
					Transaction_Scheduling_Policy = SSD_Components::Flash_Scheduling_Type::OUT_OF_ORDER;
				}
				else if (strcmp(val.c_str(), "PRIORITY_OUT_OF_ORDER") == 0)
				{
					Transaction_Scheduling_Policy = SSD_Components::Flash_Scheduling_Type::PRIORITY_OUT_OF_ORDER;
				}
				else if (strcmp(val.c_str(), "FLIN") == 0)
				{
					Transaction_Scheduling_Policy = SSD_Components::Flash_Scheduling_Type::FLIN;
				} else {
					PRINT_ERROR("Unknown transaction scheduling type specified in the SSD configuration file")
				}
			} else if (strcmp(param->name(), "Overprovisioning_Ratio") == 0) {
				std::string val = param->value();
				Overprovisioning_Ratio = std::stod(val);
				if(Overprovisioning_Ratio < 0.05) {
					PRINT_MESSAGE("The specified overprovisioning ratio is too small. The simluation may not run correctly.")
				}
			} else if (strcmp(param->name(), "GC_Exec_Threshold") == 0) {
				std::string val = param->value();
				GC_Exec_Threshold = std::stod(val);
			} else if (strcmp(param->name(), "GC_Block_Selection_Policy") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "GREEDY") == 0) {
					GC_Block_Selection_Policy = SSD_Components::GC_Block_Selection_Policy_Type::GREEDY;
				} else if (strcmp(val.c_str(), "RGA") == 0) {
					GC_Block_Selection_Policy = SSD_Components::GC_Block_Selection_Policy_Type::RGA;
				} else if (strcmp(val.c_str(), "RANDOM") == 0) {
					GC_Block_Selection_Policy = SSD_Components::GC_Block_Selection_Policy_Type::RANDOM;
				} else if (strcmp(val.c_str(), "RANDOM_P") == 0) {
					GC_Block_Selection_Policy = SSD_Components::GC_Block_Selection_Policy_Type::RANDOM_P;
				} else if (strcmp(val.c_str(), "RANDOM_PP") == 0) {
					GC_Block_Selection_Policy = SSD_Components::GC_Block_Selection_Policy_Type::RANDOM_PP;
				} else if (strcmp(val.c_str(), "FIFO") == 0) {
					GC_Block_Selection_Policy = SSD_Components::GC_Block_Selection_Policy_Type::FIFO;
				} else {
					PRINT_ERROR("Unknown GC block selection policy specified in the SSD configuration file")
				}
			} else if (strcmp(param->name(), "Use_Copyback_for_GC") == 0) {
					std::string val = param->value();
					std::transform(val.begin(), val.end(), val.begin(), ::toupper);
					Use_Copyback_for_GC = (val.compare("FALSE") == 0 ? false : true);
			} else if (strcmp(param->name(), "Preemptible_GC_Enabled") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				Preemptible_GC_Enabled = (val.compare("FALSE") == 0? false : true);
			} else if (strcmp(param->name(), "GC_Hard_Threshold") == 0) {
				std::string val = param->value();
				GC_Hard_Threshold = std::stod(val);
			} else if (strcmp(param->name(), "Dynamic_Wearleveling_Enabled") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				Dynamic_Wearleveling_Enabled = (val.compare("FALSE") == 0 ? false : true);
			} else if (strcmp(param->name(), "Static_Wearleveling_Enabled") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				Static_Wearleveling_Enabled = (val.compare("FALSE") == 0 ? false : true);
			} else if (strcmp(param->name(), "Static_Wearleveling_Threshold") == 0) {
				std::string val = param->value();
				Static_Wearleveling_Threshold = std::stoul(val);
			} else if (strcmp(param->name(), "Prefered_suspend_erase_time_for_read") == 0) {
				std::string val = param->value();
				Preferred_suspend_erase_time_for_read = std::stoull(val);
			} else if (strcmp(param->name(), "Preferred_suspend_erase_time_for_write") == 0) {
				std::string val = param->value();
				Preferred_suspend_erase_time_for_write = std::stoull(val);
			} else if (strcmp(param->name(), "Preferred_suspend_write_time_for_read") == 0) {
				std::string val = param->value();
				Preferred_suspend_write_time_for_read = std::stoull(val);
			} else if (strcmp(param->name(), "Flash_Channel_Count") == 0) {
				std::string val = param->value();
				Flash_Channel_Count = std::stoul(val);
			} else if (strcmp(param->name(), "Flash_Channel_Width") == 0) {
				std::string val = param->value();
				Flash_Channel_Width = std::stoul(val);
			} else if (strcmp(param->name(), "Channel_Transfer_Rate") == 0) {
				std::string val = param->value();
				Channel_Transfer_Rate = std::stoul(val);
			} else if (strcmp(param->name(), "Chip_No_Per_Channel") == 0) {
				std::string val = param->value();
				Chip_No_Per_Channel = std::stoul(val);
			} else if (strcmp(param->name(), "Flash_Comm_Protocol") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "NVDDR2") == 0) {
					Flash_Comm_Protocol = SSD_Components::ONFI_Protocol::NVDDR2;
				} else {
					PRINT_ERROR("Unknown flash communication protocol type specified in the SSD configuration file")
				}
			// RAID 파라미터 변수 반영 추가
			} else if (strcmp(param->name(), "SSD_Count") == 0) {
				std::string val = param->value();
				SSD_Count = std::stoul(val);
			} else if (strcmp(param->name(), "Stripe_Unit_LBA") == 0) {
				std::string val = param->value();
				Stripe_Unit_LBA = std::stoul(val);
			} else if (strcmp(param->name(), "SWANS_Enabled") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				SWANS_Enabled = (val.compare("FALSE") == 0 ? false : true);
			} else if (strcmp(param->name(), "SWANS_Zone_Size_LBA") == 0) {
				std::string val = param->value();
				SWANS_Zone_Size_LBA = std::stoul(val);
			} else if (strcmp(param->name(), "Zone_Stripe_Multiplier") == 0) {
				std::string val = param->value();
				Zone_Stripe_Multiplier = std::stoul(val);
			} else if (strcmp(param->name(), "SWANS_Epoch_Length") == 0) {
				std::string val = param->value();
				SWANS_Epoch_Length = std::stoull(val);
				SWANS_Epoch_Default = SWANS_Epoch_Length;
				SWANS_Epoch_Placement = SWANS_Epoch_Length;
				SWANS_Epoch_Migration = SWANS_Epoch_Length;
			} else if (strcmp(param->name(), "SWANS_Epoch_Default") == 0) {
				std::string val = param->value();
				SWANS_Epoch_Default = std::stoull(val);
				SWANS_Epoch_Length = SWANS_Epoch_Default;
			} else if (strcmp(param->name(), "SWANS_Epoch_Placement") == 0) {
				std::string val = param->value();
				SWANS_Epoch_Placement = std::stoull(val);
			} else if (strcmp(param->name(), "SWANS_Epoch_Migration") == 0) {
				std::string val = param->value();
				SWANS_Epoch_Migration = std::stoull(val);
			} else if (strcmp(param->name(), "SWANS_TH_Precautionary") == 0) {
				std::string val = param->value();
				SWANS_TH_Precautionary = std::stod(val);
			} else if (strcmp(param->name(), "SWANS_TH_Critical") == 0) {
				std::string val = param->value();
				SWANS_TH_Critical = std::stod(val);
			} else if (strcmp(param->name(), "SWANS_Max_Concurrent_Migrations") == 0) {
				std::string val = param->value();
				SWANS_Max_Concurrent_Migrations = std::stoul(val);
			} else if (strcmp(param->name(), "SWANS_Migration_Buffer_Limit") == 0) {
				std::string val = param->value();
				SWANS_Migration_Buffer_Limit = std::stoul(val);
				SWANS_Migration_Working_Queue_Limit = SWANS_Migration_Buffer_Limit;
			} else if (strcmp(param->name(), "SWANS_Migration_Working_Queue_Limit") == 0) {
				std::string val = param->value();
				SWANS_Migration_Working_Queue_Limit = std::stoul(val);
			} else if (strcmp(param->name(), "SWANS_Buffered_Write_Completion_Mode") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				SWANS_Buffered_Write_Completion_Mode = val;
			} else if (strcmp(param->name(), "Bad_Block_Retirement_Enabled") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				Bad_Block_Retirement_Enabled = (val.compare("FALSE") == 0 ? false : true);
			} else if (strcmp(param->name(), "Simulation_Stop_Mode") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				Simulation_Stop_Mode = val;
			} else if (strcmp(param->name(), "End_of_Life_Threshold") == 0) {
				std::string val = param->value();
				End_of_Life_Threshold = std::stod(val);
			}
			else if (strcmp(param->name(), "Flash_Parameter_Set") == 0)
			{
				Flash_Parameters.XML_deserialize(param);
			}
		}
	}
	catch (...)
	{
		PRINT_ERROR("Error in Device_Parameter_Set!")
	}
}
