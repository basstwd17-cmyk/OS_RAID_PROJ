"""Small real NAND workload: exercise bad-block sampling, trigger, and XML output."""
from pathlib import Path
import random
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1]).resolve()
OUT = ROOT / "build" / "wear-smoke"
OUT.mkdir(parents=True, exist_ok=True)


def run(name, retirement=True, metric="BAD_BLOCK_COUNT", omit_metric=False):
    folder = OUT / name
    folder.mkdir(exist_ok=True)
    tree = ET.parse(ROOT / "ssdconfig.xml")
    settings = {
        "Flash_Channel_Count": 1, "Chip_No_Per_Channel": 1,
        "Die_No_Per_Chip": 1, "Plane_No_Per_Die": 1,
        "Block_No_Per_Plane": 128, "Page_No_Per_Block": 8,
        "Block_PE_Cycles_Limit": 3, "Overprovisioning_Ratio": 0.3,
        "End_of_Life_Threshold": 0.05, "GC_Exec_Threshold": 0.15,
        "Stripe_Unit_LBA": 64, "SWANS_Zone_Size_LBA": 64,
        "SWANS_Epoch_Default": 1000000, "SWANS_Epoch_Placement": 1000000,
        "SWANS_Epoch_Migration": 1000000, "SWANS_TH_Precautionary": 1e12,
        "SWANS_TH_Critical": 0, "RAID_Telemetry_Enabled": "false",
        "Bad_Block_Retirement_Enabled": str(retirement).lower(),
        "SWANS_Wear_Metric": metric, "IO_Queue_Depth": 16, "Queue_Fetch_Size": 16,
    }
    for key, value in settings.items():
        tree.find(".//" + key).text = str(value)
    if omit_metric:
        device = tree.find(".//Device_Parameter_Set")
        device.remove(device.find("SWANS_Wear_Metric"))
    tree.write(folder / "ssd.xml")
    rng = random.Random(49)
    with (folder / "io.trace").open("w") as trace:
        for i in range(16000 if retirement else 1500):
            # Only SSD 0 initially; many live zones and free zones on other SSDs.
            lba = rng.randrange(72) * 4 * 64 + rng.randrange(8) * 8
            trace.write(f"{i * 2000000} 0 {lba} 8 0\n")
    scenario = ET.Element("MQSim_IO_Scenarios")
    flow = ET.SubElement(ET.SubElement(scenario, "IO_Scenario"), "IO_Flow_Parameter_Set_Trace_Based")
    fields = {
        "Priority_Class": "HIGH", "Device_Level_Data_Caching_Mode": "TURNED_OFF",
        "Channel_IDs": "0", "Chip_IDs": "0", "Die_IDs": "0", "Plane_IDs": "0",
        "Initial_Occupancy_Percentage": "0", "File_Path": str(folder / "io.trace"),
        "Percentage_To_Be_Executed": "100", "Relay_Count": "1", "Time_Unit": "NANOSECOND",
    }
    for key, value in fields.items():
        ET.SubElement(flow, key).text = value
    ET.ElementTree(scenario).write(folder / "workload.xml")
    result = subprocess.run([str(BINARY), "-i", str(folder / "ssd.xml"), "-w", str(folder / "workload.xml")],
                            input="\n", text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            cwd=folder, timeout=90)
    (folder / "run.log").write_text(result.stdout)
    if metric != "BAD_BLOCK_COUNT":
        assert result.returncode != 0 and "Unsupported SWANS_Wear_Metric" in result.stdout, result.stdout[-2000:]
        return
    assert result.returncode == 0, result.stdout[-2000:]
    xml = ET.parse(folder / "workload_scenario_1.xml").getroot()
    policy = next(e for e in xml.iter() if e.tag.endswith(".SWANSPolicyStatistics"))
    assert policy.findtext("Migration_Wear_Metric") == "BAD_BLOCK_COUNT"
    assert policy.findtext("Migration_Trigger") == "(Wear_avg + Wear_max) / 2 < Wear_i"
    records = [e for e in xml.iter() if e.tag.startswith("SWANS_Migration_Record_")]
    if retirement:
        assert records, "Expected a physical-wear-triggered migration; see " + str(folder / "run.log")
        for record in records:
            source = float(record.findtext("Source_Wear"))
            target = float(record.findtext("Destination_Wear"))
            average = float(record.findtext("Wear_Average"))
            threshold = float(record.findtext("Wear_Threshold"))
            assert source > threshold and source > target
            assert abs(threshold - (average + 1) / 2) < 1e-6
        assert any(e.tag == "Bad_Block_Count" and int(e.text) > 0 for e in xml.iter())
        print(f"{name}: {len(records)} real migrations, all satisfy wear trigger")
    else:
        assert not records
        assert policy.findtext("Last_Evaluation_Wear_Valid") == "false"
        print(f"{name}: no migration without valid retirement metric")


run("bad-block-explicit")
run("retirement-disabled", retirement=False)
run("unsupported-tail", metric="TAIL_ERASE_COUNT")
print("wear trigger simulator smoke tests passed")
