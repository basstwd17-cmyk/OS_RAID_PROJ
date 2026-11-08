"""Bounded real-NAND checks for the TEC/PWL on-demand zone policy. Run from any directory."""
from pathlib import Path
import argparse
import json
import random
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
PERIOD = 100000000


def setting(tree, key, value):
    device = tree.find('.//Device_Parameter_Set')
    node = device.find('.//' + key)
    if node is None:
        node = ET.SubElement(device, key)
    node.text = str(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'MQSim.exe')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/lazy-validation')
    args = parser.parse_args()
    binary, output = args.binary.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    results = []
    for name, reserve, mode in [('empty', False, 'ON_DEMAND'), ('exchange', True, 'ON_DEMAND'), ('eager-control', True, 'EAGER')]:
        folder = output / name
        folder.mkdir()
        tree = ET.parse(ROOT / 'ssdconfig.xml')
        values = {
            'SSD_Count':4, 'Enabled_Preconditioning':'false', 'Flash_Channel_Count':1,
            'Chip_No_Per_Channel':1, 'Die_No_Per_Chip':1, 'Plane_No_Per_Die':1,
            'Block_No_Per_Plane':128, 'Page_No_Per_Block':8, 'Page_Capacity':4096,
            'Block_PE_Cycles_Limit':3, 'Overprovisioning_Ratio':.3, 'End_of_Life_Threshold':.05,
            'GC_Exec_Threshold':.15, 'IO_Queue_Depth':16, 'Queue_Fetch_Size':16,
            'Stripe_Unit_LBA':64, 'SWANS_Zone_Size_LBA':64, 'SWANS_Enabled':'true',
            'Data_Cache_Capacity':0, 'SWANS_Migration_Mode':mode,
            'op_zone_migration':str(reserve).lower(), 'SWANS_Wear_Metric':'TEC',
            'SWANS_Migration_Trigger':'DYNAMIC_WEAR', 'SWANS_Zone_Selection':'SSD_POPULARITY',
            'SWANS_Max_Concurrent_Migrations':1, 'SWANS_Migration_Working_Queue_Limit':4,
            'SWANS_TEC_TH_Precautionary_PctPoint':40, 'SWANS_TEC_TH_Critical_PctPoint':45,
            'SWANS_Epoch_Default':PERIOD, 'SWANS_Epoch_Placement':PERIOD,
            'SWANS_Epoch_Migration':PERIOD, 'SWANS_Zone_Popularity_Mode':'HALF_EPOCH',
            'SWANS_Zone_Popularity_Period':PERIOD, 'RAID_Telemetry_Period':PERIOD,
            'RAID_Telemetry_Enabled':'true',
        }
        for key, value in values.items():
            setting(tree, key, value)
        tree.write(folder / 'ssd.xml')
        rng = random.Random(49)
        rows = []
        if reserve:
            rows += [(z*64, 8, 0) for z in range(88*4)]
        for i in range(6000):
            z = 0 if i % 5 == 0 else rng.randrange(72)*4
            if reserve and i % 19 == 0:
                z = (i//19 % 88)*4 + 1 + i//19 % 3
            page = rng.randrange(8)
            partial = i % 7 == 0
            rows.append((z*64 + page*8 + (2 if partial else 0), 4 if partial else 8, 1 if i % 11 == 0 else 0))
        trace = folder / 'io.trace'
        trace.write_text(''.join(f'{(i+1)*2000000} 0 {lba} {count} {op}\n' for i,(lba,count,op) in enumerate(rows)), encoding='ascii')
        workload = ET.Element('MQSim_IO_Scenarios')
        flow = ET.SubElement(ET.SubElement(workload,'IO_Scenario'),'IO_Flow_Parameter_Set_Trace_Based')
        for key, value in {
            'Priority_Class':'HIGH','Device_Level_Data_Caching_Mode':'TURNED_OFF',
            'Channel_IDs':0,'Chip_IDs':0,'Die_IDs':0,'Plane_IDs':0,
            'Initial_Occupancy_Percentage':0,'File_Path':trace.as_posix(),
            'Percentage_To_Be_Executed':100,'Relay_Count':0,'Time_Unit':'NANOSECOND',
        }.items():
            ET.SubElement(flow,key).text = str(value)
        ET.ElementTree(workload).write(folder/'workload.xml')
        p = subprocess.run([str(binary),'-i',str(folder/'ssd.xml'),'-w',str(folder/'workload.xml')],
                           cwd=folder,input='\n',text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
        (folder/'run.log').write_text(p.stdout,encoding='utf-8')
        assert p.returncode == 0, p.stdout[-3000:]
        report = ET.parse(folder/'workload_scenario_1.xml').getroot()
        def value(key): return next(report.iter(key)).text
        assert value('Termination_Reason') == 'EOL'
        assert value('SWANS_Mapping_Duplicate_Physical_Locations') == '0'
        assert value('SWANS_Migration_Mode') == mode
        assert value('SWANS_Zone_Popularity_Update') == 'LIVE_SECTORS_HALF_PER_FIXED_EPOCH'
        records = [e for e in report.iter() if e.tag.startswith('SWANS_Migration_Record_')]
        completed = [e for e in records if e.findtext('Status') == 'COMPLETED']
        assert completed, (name,'no completed migration')
        if mode == 'ON_DEMAND':
            assert all(e.findtext('Status') != 'ACTIVE' for e in records), 'EOL must mark unfinished tasks aborted'
        for record in records:
            assert float(record.findtext('Source_Wear')) > float(record.findtext('Wear_Threshold'))
        if mode == 'ON_DEMAND':
            starts = [int(e.findtext('Start_Time')) for e in records]
            assert all(t % PERIOD == 0 for t in starts), starts
            assert len(set(starts)) == len(starts)
            assert int(value('SWANS_Lazy_Offloaded_Pages')) > 0
            assert int(value('SWANS_Lazy_Deadline_Drains')) > 0
        if reserve:
            assert all(e.findtext('Mode') == 'op_zone_migration' for e in records)
            assert int(value('SWANS_Exchange_Cold_Stages_Completed')) >= len(completed)
        result = dict(test=name,completed_migrations=len(completed),offloaded_pages=int(value('SWANS_Lazy_Offloaded_Pages')),
                      copied_pages=int(value('SWANS_Lazy_Copied_Pages')),deadline_drains=int(value('SWANS_Lazy_Deadline_Drains')),
                      cold_write_drains=int(value('SWANS_Lazy_Cold_Write_Drains')))
        results.append(result)
        print(json.dumps(result),flush=True)
        if name == 'exchange':
            # Use the selected cold zone from the deterministic baseline, then
            # inject a real conflicting host write after the reverse copy.
            first = records[0]
            start = int(first.findtext('Start_Time'))
            cold = int(first.findtext('Cold_Zone'))
            conflict_trace = folder / 'conflict.trace'
            records_text = [line.split() for line in trace.read_text().splitlines()]
            records_text.append([str(start + PERIOD//3),'0',str(cold*64),'8','0'])
            records_text.sort(key=lambda row:int(row[0]))
            conflict_trace.write_text(''.join(' '.join(row)+'\n' for row in records_text),encoding='ascii')
            conflict_workload = ET.ElementTree(workload)
            conflict_workload.find('.//File_Path').text = conflict_trace.as_posix()
            conflict_workload.write(folder/'conflict.xml')
            pc = subprocess.run([str(binary),'-i',str(folder/'ssd.xml'),'-w',str(folder/'conflict.xml')],
                                cwd=folder,input='\n',text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
            (folder/'conflict.log').write_text(pc.stdout,encoding='utf-8')
            assert pc.returncode == 0, pc.stdout[-2000:]
            conflict = ET.parse(folder/'conflict_scenario_1.xml').getroot()
            drains = int(next(conflict.iter('SWANS_Lazy_Cold_Write_Drains')).text)
            assert drains > 0
            cr = next(e for e in conflict.iter() if e.tag.startswith('SWANS_Migration_Record_'))
            assert cr.findtext('Status') == 'COMPLETED' and int(cr.findtext('End_Time')) < start + PERIOD
            assert next(conflict.iter('SWANS_Mapping_Duplicate_Physical_Locations')).text == '0'
            results.append(dict(test='cold-write-conflict',cold_write_drains=drains,first_end=int(cr.findtext('End_Time'))))
            print(json.dumps(results[-1]),flush=True)
        if name == 'empty':
            # Reject incompatible geometry and multi-task settings before host I/O.
            for key, val in [('SWANS_Max_Concurrent_Migrations',2),('Stripe_Unit_LBA',32),('Enabled_Preconditioning','true'),('SWANS_Migration_Mode','INVALID')]:
                bad = ET.parse(folder/'ssd.xml')
                setting(bad,key,val)
                bad.write(folder/'invalid.xml')
                pbad = subprocess.run([str(binary),'-i',str(folder/'invalid.xml'),'-w',str(folder/'workload.xml')],
                                      cwd=folder,input='\n',text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=10)
                expected_error = 'Error in Device_Parameter_Set' if key == 'SWANS_Migration_Mode' else 'ON_DEMAND requires'
                assert pbad.returncode != 0 and expected_error in pbad.stdout, pbad.stdout[-1200:]
            print('invalid lazy configurations rejected',flush=True)
    (output/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')

if __name__ == '__main__':
    main()
