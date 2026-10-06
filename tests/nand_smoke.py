"""Run a small SWANS migration through the real SSD/cache/FTL/NAND path."""
from pathlib import Path
import os
import subprocess
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
output = root / "build" / "nand-smoke"
output.mkdir(parents=True, exist_ok=True)
device = ET.parse(root / "ssdconfig.xml")
overrides = {
    "SSD_Count": 2, "Flash_Channel_Count": 1, "Chip_No_Per_Channel": 1,
    "Die_No_Per_Chip": 1, "Plane_No_Per_Die": 1, "Block_No_Per_Plane": 128,
    "Page_No_Per_Block": 8, "Page_Capacity": 4096, "Stripe_Unit_LBA": 64,
    "SWANS_Zone_Size_LBA": 64, "SWANS_Epoch_Default": 8000000,
    "SWANS_Epoch_Placement": 8000000, "SWANS_Epoch_Migration": 8000000,
    "SWANS_TH_Precautionary": 2.5, "SWANS_TH_Critical": 3,
    "SWANS_Balance_Unit_Bytes": 4096, "SWANS_Migration_Working_Queue_Limit": 2,
    "Bad_Block_Retirement_Enabled": "false", "RAID_Telemetry_Enabled": "false",
    "IO_Queue_Depth": 32, "Queue_Fetch_Size": 16,
}
for name, value in overrides.items():
    node = device.find(".//" + name)
    assert node is not None, name
    node.text = str(value)
device.write(output / "device.xml", encoding="utf-8")

trace = [(1000 + i * 500000, i * 8, 8, 0) for i in range(8)]
trace += [(8100000, 0, 8, 1), (8200000, 0, 8, 0), (8300000, 8, 8, 0),
          (8400000, 16, 8, 0), (10000000, 8, 8, 1), (11000000, 0, 16, 1),
          (20000000, 0, 8, 1)]
(output / "mixed.trace").write_text(
    "".join(f"{time} 0 {lba} {size} {kind}\n" for time, lba, size, kind in trace),
    encoding="ascii",
)
workload = ET.Element("MQSim_IO_Scenarios")
flow = ET.SubElement(ET.SubElement(workload, "IO_Scenario"), "IO_Flow_Parameter_Set_Trace_Based")
for name, value in {
    "Priority_Class": "HIGH", "Device_Level_Data_Caching_Mode": "TURNED_OFF",
    "Channel_IDs": "0", "Chip_IDs": "0", "Die_IDs": "0", "Plane_IDs": "0",
    "Initial_Occupancy_Percentage": "0", "File_Path": str(output / "mixed.trace"),
    "Percentage_To_Be_Executed": "100", "Relay_Count": "1", "Time_Unit": "NANOSECOND",
}.items():
    ET.SubElement(flow, name).text = value
ET.ElementTree(workload).write(output / "workload.xml", encoding="utf-8")
binary = root / ("MQSim.exe" if os.name == "nt" else "MQSim")
with (output / "run.log").open("w", encoding="utf-8") as log:
    subprocess.run([str(binary), "-i", str(output / "device.xml"), "-w", str(output / "workload.xml")],
                   cwd=root, input="\n", text=True, stdout=log, stderr=subprocess.STDOUT,
                   timeout=30, check=True)
result = ET.parse(output / "workload_scenario_1.xml")
expected = {
    "Submitted_Requests": "15", "Completed_Requests": "15",
    "SWANS_Migration_Operations": "1", "SWANS_Read_Pause_Requests": "3",
    "SWANS_Buffered_Requests": "2", "SWANS_Buffered_Write_Completions": "2",
    "SWANS_Backpressure_Events": "1", "SWANS_Blocked_Requests": "0",
    "SWANS_Dirty_Buffer_Blocks": "0", "SWANS_Migration_Active": "false",
    "SWANS_Migrating_Zones": "0", "SWANS_Mapping_Duplicate_Physical_Locations": "0",
    "SWANS_Write_During_Migration_Mode": "CACHE_AND_REPLAY",
    "SWANS_Read_During_Migration_Mode": "PAUSE_AT_IO_BOUNDARY",
}
for name, value in expected.items():
    actual = result.findtext(".//" + name)
    assert actual == value, f"{name}: expected {value}, got {actual}"
print("PASS: NAND smoke, 15/15 completions, 3 priority reads, 2 cached writes, 1 overflow write")
