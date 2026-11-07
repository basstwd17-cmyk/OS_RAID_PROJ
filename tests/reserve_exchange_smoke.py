"""End-to-end reserve exchange and separate SWANS XML validation."""
from pathlib import Path
import copy
import random
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1]).resolve()
OUT = ROOT / "build" / "reserve-smoke"
OUT.mkdir(parents=True, exist_ok=True)


def invoke(folder, extra=None, expected=None):
    command = [str(BINARY), "-i", str(folder / "ssd.xml"), "-w", str(folder / "workload.xml")]
    command += extra if extra is not None else ["-s", str(folder / "swans.xml")]
    result = subprocess.run(command, input="\n", text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, cwd=folder, timeout=90)
    (folder / "run.log").write_text(result.stdout)
    if expected:
        assert result.returncode != 0 and expected in result.stdout, result.stdout[-2000:]
        return
    assert result.returncode == 0, result.stdout[-3000:]
    return ET.parse(folder / "workload_scenario_1.xml").getroot()


def prepare(name, precondition=False, limits=False, metric="BAD_BLOCK_COUNT"):
    folder = OUT / name
    folder.mkdir(exist_ok=True)
    ssd = ET.parse(ROOT / "ssdconfig.xml")
    settings = {
        "Flash_Channel_Count": 1, "Chip_No_Per_Channel": 1,
        "Die_No_Per_Chip": 1, "Plane_No_Per_Die": 1,
        "Block_No_Per_Plane": 128, "Page_No_Per_Block": 8,
        "Block_PE_Cycles_Limit": 3, "Overprovisioning_Ratio": 0.3,
        "End_of_Life_Threshold": 0.05, "GC_Exec_Threshold": 0.15,
        "Stripe_Unit_LBA": 64, "SWANS_Zone_Size_LBA": 64,
        "SWANS_Enabled": "false",  # separate XML must override this
        "RAID_Telemetry_Enabled": "false", "Enabled_Preconditioning": str(precondition).lower(),
        "IO_Queue_Depth": 16, "Queue_Fetch_Size": 16,
    }
    for key, value in settings.items():
        ssd.find(".//" + key).text = str(value)
    ssd.write(folder / "ssd.xml")
    swans = ET.Element("SWANS_Parameter_Set")
    for key, value in {
        "SWANS_Enabled": "true", "op_zone_migration": "true",
        "SWANS_Wear_Metric": metric,
        "SWANS_Migration_Trigger": "IMBALANCE" if metric == "WRITE_BYTES" else "DYNAMIC_WEAR",
        "SWANS_TH_Critical": 32, "SWANS_Balance_Unit_Bytes": 4096,
        "SWANS_TEC_TH_Precautionary_PctPoint": 40,
        "SWANS_TEC_TH_Critical_PctPoint": 45,
        "SWANS_Epoch_Default": 1000000, "SWANS_Epoch_Placement": 1000000,
        "SWANS_Epoch_Migration": 1000000, "SWANS_TH_Precautionary": "1e12",
        "SWANS_Zone_Popularity_Period": 100000000, "SWANS_Zone_Popularity_Mode": "HALF_DECAY",
        "SWANS_Migration_Slice_Sectors": 64 if limits else 0,
        "SWANS_Migration_Slice_Time": 1000000 if limits else 0,
        "SWANS_Migration_Pause_Duration": 100000,
        "SWANS_Migration_Pause_On_Read": str(limits).lower(),
    }.items():
        ET.SubElement(swans, key).text = str(value)
    ET.ElementTree(swans).write(folder / "swans.xml")
    # Backend 5734 sectors/SSD; export 88 * 64 sectors, reserve 1 full zone.
    rng = random.Random(49)
    with (folder / "io.trace").open("w") as trace:
        i = 0
        # Initialize every visible zone (all 4 SSDs), leaving only hidden reserves empty.
        for zone in range(88 * 4):
            trace.write(f"{i * 20000000} 0 {zone * 64} 8 0\n")
            i += 1
        for j in range(16000):
            lba = rng.randrange(72) * 4 * 64 + rng.randrange(8) * 8
            if limits and j % 5 == 0:
                trace.write(f"{i * 20000000} 0 128 8 1\n")
            else:
                trace.write(f"{i * 20000000} 0 {lba} 8 0\n")
            i += 1
    scenarios = ET.Element("MQSim_IO_Scenarios")
    scenario = ET.SubElement(scenarios, "IO_Scenario")
    flow = ET.SubElement(scenario, "IO_Flow_Parameter_Set_Trace_Based")
    for key, value in {
        "Priority_Class": "HIGH", "Device_Level_Data_Caching_Mode": "TURNED_OFF",
        "Channel_IDs": "0", "Chip_IDs": "0", "Die_IDs": "0", "Plane_IDs": "0",
        "Initial_Occupancy_Percentage": "50" if precondition else "0",
        "File_Path": str(folder / "io.trace"), "Percentage_To_Be_Executed": "100",
        "Relay_Count": "1", "Time_Unit": "NANOSECOND",
    }.items():
        ET.SubElement(flow, key).text = value
    ET.ElementTree(scenarios).write(folder / "workload.xml")
    return folder


def verify(name, precondition=False, limits=False, metric="BAD_BLOCK_COUNT"):
    folder = prepare(name, precondition, limits, metric)
    root = invoke(folder)
    def value(key):
        return next(e.text for e in root.iter(key))
    assert value("SWANS_User_Zone_Count") == "352"
    assert value("SWANS_Reserve_Zone_Count") == "4"
    assert value("SWANS_Mapping_Duplicate_Physical_Locations") == "0"
    assert value("SWANS_Zone_Popularity_Mode") == "HALF_DECAY"
    assert value("SWANS_Zone_Popularity_Period") == "0"
    assert value("SWANS_Zone_Popularity_Update") == "LIVE_WRITES_HALF_ALL_ZONES_PER_COMPLETED_MIGRATION"
    records = [e for e in root.iter() if e.tag.startswith("SWANS_Migration_Record_")]
    assert records, (folder, "No migration")
    assert all(e.findtext("Mode") == "op_zone_migration" for e in records)
    completed = [e for e in records if e.findtext("Status") == "COMPLETED"]
    assert completed, (folder, "No completed exchange")
    for e in records:
        assert int(e.findtext("Cold_Copy_Sectors")) > 0
        if metric != "WRITE_BYTES":
            assert float(e.findtext("Source_Wear")) > float(e.findtext("Wear_Threshold"))
    assert int(value("SWANS_Exchange_Cold_Stages_Completed")) >= len(completed)
    if limits:
        assert int(value("SWANS_Migration_Time_Pauses")) + int(value("SWANS_Migration_Sector_Pauses")) > 0
    print(f"{name}: {len(completed)} completed reserve exchanges; 352 user / 4 reserve zones")


verify("full-device")
verify("sliced", limits=True)
verify("tec-exchange", metric="TEC")
verify("write-exchange", metric="WRITE_BYTES")
folder = prepare("legacy-reserve-option")
legacy = (folder / "swans.xml").read_text().replace("op_zone_migration", "SWANS_Reserve_Zone_Enabled")
(folder / "swans.xml").write_text(legacy)
root = invoke(folder)
assert next(root.iter("SWANS_Reserve_Zone_Count")).text == "4"
assert any(e.findtext("Mode") == "op_zone_migration" for e in root.iter()
           if e.tag.startswith("SWANS_Migration_Record_"))
# The same option is accepted in the SSD XML, without a separate -s file.
folder = prepare("inline-op-option")
ssd = ET.parse(folder / "ssd.xml")
device = ssd.find(".//Device_Parameter_Set")
for node in ET.parse(folder / "swans.xml").getroot():
    existing = device.find(node.tag)
    if existing is not None:
        existing.text = node.text
    else:
        device.append(copy.deepcopy(node))
ssd.write(folder / "ssd.xml")
root = invoke(folder, [])
assert next(root.iter("SWANS_Reserve_Zone_Count")).text == "4"
assert any(e.findtext("Mode") == "op_zone_migration" for e in root.iter()
           if e.tag.startswith("SWANS_Migration_Record_"))
print("op_zone_migration: named option, legacy alias and inline SSD XML passed")
# Preserve matching legacy aliases, but never let XML ordering choose capacity.
inline_xml = (folder / "ssd.xml").read_bytes()
for i, (pairs, count) in enumerate([
    ([("op_zone_migration", "true"), ("SWANS_Reserve_Zone_Enabled", "false")], None),
    ([("SWANS_Reserve_Zone_Enabled", "false"), ("op_zone_migration", "true")], None),
    ([("op_zone_migration", "true"), ("op_zone_migration", "false")], None),
    ([("op_zone_migration", "false"), ("op_zone_migration", "true")], None),
    ([("SWANS_Reserve_Zone_Enabled", "true"), ("SWANS_Reserve_Zone_Enabled", "true")], None),
    ([("op_zone_migration", "true"), ("SWANS_Reserve_Zone_Enabled", "true")], 4),
    ([("SWANS_Reserve_Zone_Enabled", "false"), ("op_zone_migration", "false")], 0),
]):
    ssd = ET.ElementTree(ET.fromstring(inline_xml))
    device = ssd.find(".//Device_Parameter_Set")
    for key in ("op_zone_migration", "SWANS_Reserve_Zone_Enabled"):
        for node in device.findall(key):
            device.remove(node)
    for key, value in pairs:
        ET.SubElement(device, key).text = value
    ssd.write(folder / "ssd.xml")
    if count is None:
        invoke(folder, [], expected="Error in Device_Parameter_Set")
    else:
        root = invoke(folder, [])
        assert next(root.iter("SWANS_Reserve_Zone_Count")).text == str(count)
(folder / "ssd.xml").write_bytes(inline_xml)
print("inline OP aliases: conflicts/duplicates rejected; matching aliases preserved")
folder = prepare("preconditioned", precondition=True)
# Trace preconditioning is unsupported by the existing FTL; use its supported
# uniform synthetic workload to verify registration of initially mapped pages.
sample = ET.parse(ROOT / "workload.xml").find(".//IO_Flow_Parameter_Set_Synthetic")
for key, value in {
    "Channel_IDs": "0", "Chip_IDs": "0", "Die_IDs": "0", "Plane_IDs": "0",
    "Device_Level_Data_Caching_Mode": "TURNED_OFF", "Initial_Occupancy_Percentage": "50",
    "Working_Set_Percentage": "100", "Read_Percentage": "100",
    "Total_Requests_To_Generate": "100", "Stop_Time": "0",
    "Average_No_of_Reqs_in_Queue": "1", "Average_Request_Size": "8",
}.items():
    sample.find(key).text = value
scenarios = ET.Element("MQSim_IO_Scenarios")
ET.SubElement(scenarios, "IO_Scenario").append(sample)
ET.ElementTree(scenarios).write(folder / "workload.xml")
root = invoke(folder)
assert int(next(root.iter("SWANS_Preconditioned_Valid_Sectors")).text) > 0
assert next(root.iter("SWANS_Exported_Sectors")).text == str(352 * 64)
print("preconditioned: initial valid pages registered; hidden capacity excluded")

folder = prepare("invalid-config")
original = (folder / "swans.xml").read_text()
cases = [
    ("<SWANS_Parameter_Set><op_zone_migration>yes</op_zone_migration></SWANS_Parameter_Set>", "true or false"),
    ("<SWANS_Parameter_Set><op_zone_migration>true</op_zone_migration><SWANS_Reserve_Zone_Enabled>false</SWANS_Reserve_Zone_Enabled></SWANS_Parameter_Set>", "Duplicate"),
    ("<SWANS_Parameter_Set><Unknown>1</Unknown></SWANS_Parameter_Set>", "Unknown"),
    ("<SWANS_Parameter_Set><SWANS_Enabled>true</SWANS_Enabled><SWANS_Enabled>false</SWANS_Enabled></SWANS_Parameter_Set>", "Duplicate"),
    ("<SWANS_Parameter_Set><SWANS_Migration_Slice_Sectors>-1</SWANS_Migration_Slice_Sectors></SWANS_Parameter_Set>", "nonnegative"),
    ("<SWANS_Parameter_Set><SWANS_Enabled>yes</SWANS_Enabled></SWANS_Parameter_Set>", "true or false"),
    ("<SWANS_Parameter_Set><SWANS_Migration_Slice_Time>1</SWANS_Migration_Slice_Time><SWANS_Migration_Pause_Duration>0</SWANS_Migration_Pause_Duration></SWANS_Parameter_Set>", "must be positive"),
    ("<SWANS_Parameter_Set><SWANS_Wear_Metric>TAIL_ERASE_COUNT</SWANS_Wear_Metric></SWANS_Parameter_Set>", "Unsupported"),
    ("<SWANS_Parameter_Set><SWANS_Enabled>true</Wrong></SWANS_Parameter_Set>", "invalid closing tag"),
]
for xml, expected in cases:
    (folder / "swans.xml").write_text(xml)
    invoke(folder, expected=expected)
(folder / "swans.xml").write_text(original)
invoke(folder, ["-s", str(folder / "missing.xml")], "Cannot open SWANS")
invoke(folder, ["-s"], "incomplete option")
invoke(folder, ["-w", "duplicate.xml"], "duplicate")
workload = ET.parse(folder / "workload.xml")
scenario = workload.getroot().find("IO_Scenario")
scenario.append(copy.deepcopy(scenario[0]))
workload.write(folder / "workload.xml")
invoke(folder, expected="one I/O flow")
print("separate SWANS configuration: overlay, validation and CLI tests passed")
