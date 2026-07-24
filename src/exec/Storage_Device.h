#ifndef STORAGE_DEVICE_H
#define STORAGE_DEVICE_H

#include <string>
#include <vector>
#include "../host/IO_Flow_Base.h"
#include "../host/PCIe_Switch.h"
#include "../host/SATA_HBA.h"
#include "../ssd/Host_Interface_Base.h"
#include "../utils/Workload_Statistics.h"

namespace Utils
{
	class XmlWriter;
}

class Storage_Device
{
public:
	virtual ~Storage_Device() = default;
	virtual SSD_Components::Host_Interface_Base* Get_host_interface() = 0;
	virtual void Attach_to_host(Host_Components::PCIe_Switch* pcie_switch) = 0;
	virtual void Perform_preconditioning(std::vector<Utils::Workload_Statistics*> workload_stats) = 0;
	virtual void Initialize_io_streams(const std::vector<Host_Components::IO_Flow_Base*>& io_flows,
		Host_Components::SATA_HBA* sata_hba) = 0;
	virtual void Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter) = 0;
	virtual unsigned int Get_no_of_LHAs_in_an_NVM_write_unit() = 0;
	virtual LPA_type Convert_host_logical_address_to_device_address(LHA_type lha) = 0;
	virtual page_status_type Find_NVM_subunit_access_bitmap(LHA_type lha) = 0;
	virtual void Discard_logical_range(stream_id_type stream_id, LHA_type start_lha, unsigned int lha_count) {}
};

#endif
