"""Bounded real-NAND checks for the TEC/PWL on-demand zone policy. Run from any directory."""
from pathlib import Path
import argparse
import csv
import json
import math
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


def check_lazy_start_times(records, period):
    anchor = 0
    previous_end = -1
    for record in records:
        start = int(record.findtext('Start_Time'))
        assert start >= previous_end
        if start == previous_end:
            anchor = start
        else:
            assert (start - anchor) % period == 0, (start, anchor, period)
        previous_end = int(record.findtext('End_Time'))


def check_migration_csv(folder, report, workload='workload'):
    records = [node for node in report.iter() if node.tag.startswith('SWANS_Migration_Record_')]
    with (folder / f'{workload}_scenario_1.migrations.csv').open() as source:
        rows = list(csv.DictReader(source))
    device = ET.parse(folder / 'ssd.xml')
    ssd_count = int(device.findtext('.//SSD_Count'))
    blocks = math.prod(int(device.findtext('.//' + key)) for key in
                       ('Flash_Channel_Count', 'Chip_No_Per_Channel', 'Die_No_Per_Chip',
                        'Plane_No_Per_Die', 'Block_No_Per_Plane'))
    balance_unit = int(device.findtext('.//SWANS_Balance_Unit_Bytes'))
    events = {}
    for row in rows:
        assert None not in row and all(value is not None for value in row.values()), row
        sequence = int(row['migration_sequence'])
        record = records[sequence]
        events.setdefault(int(row['sample']), []).append(row)
        assert row['source_ssd'] == record.findtext('Hot_SSD')
        assert row['destination_ssd'] == record.findtext('Cold_SSD')
        assert row['source_zone'] == record.findtext('Hot_Zone')
        assert row['destination_zone'] == record.findtext('Cold_Zone')
        if record.findtext('Reserve_Zone') == 'NONE':
            assert row['space_mode'] == 'EMPTY_DESTINATION'
            assert row['reserve_zone'] == row['reverse_source_zone'] == row['reverse_destination_zone'] == ''
        else:
            assert row['space_mode'] == 'RESERVE_EXCHANGE'
            assert row['reverse_source_ssd'] == row['destination_ssd']
            assert row['reverse_destination_ssd'] == row['source_ssd']
            assert row['reverse_source_zone'] == row['destination_zone']
            assert row['reverse_destination_zone'] == row['reserve_zone'] == record.findtext('Reserve_Zone')
        assert row['execution_mode'] == record.findtext('Execution_Mode')
        assert float(row['start_time_s']) == int(record.findtext('Start_Time')) / 1e9
        assert math.isclose(float(row['time_s']), int(row['time_ns']) / 1e9)
        assert math.isclose(float(row['host_write_units']),
                            int(row['host_write_bytes_attributed']) / balance_unit)
        assert math.isclose(float(row['block_erase_mean']) * blocks,
                            int(row['completed_nand_block_erases']), abs_tol=1e-9)
        assert float(row['block_erase_min']) <= float(row['block_erase_mean']) <= float(row['block_erase_max'])
        if row['tec_valid'] == '1':
            assert math.isclose(float(row['tec_wear_percent']),
                                100 * int(row['tec']) / int(row['erase_limit']))
        if row['event'] == 'migration_start':
            assert row['status'] == 'ACTIVE' and row['forced_drain'] == '0'
            assert row['duration_s'] == '' and row['end_time_s'] == ''
        elif row['event'] == 'migration_force':
            assert row['forced_drain'] == '1' and row['force_reason'] != 'NONE'
        elif row['event'] == 'migration_end':
            assert row['status'] == 'COMPLETED'
            duration = (int(record.findtext('End_Time')) - int(record.findtext('Start_Time'))) / 1e9
            assert math.isclose(float(row['duration_s']), duration)
            assert row['force_reason'] == record.findtext('Force_Reason')
        elif row['event'] == 'migration_abort':
            assert row['status'] == 'ABORTED_EOL' and row['duration_s'] == ''
        else:
            raise AssertionError(row['event'])
        if sequence and row['event'] == 'migration_start':
            previous = records[sequence - 1]
            assert int(row['previous_migration_sequence']) == sequence - 1
            assert math.isclose(float(row['seconds_since_previous_start']),
                                (int(record.findtext('Start_Time')) - int(previous.findtext('Start_Time'))) / 1e9)
            if previous.findtext('Status') == 'COMPLETED':
                assert math.isclose(float(row['seconds_since_previous_end']),
                                    (int(record.findtext('Start_Time')) - int(previous.findtext('End_Time'))) / 1e9)
    for event in events.values():
        assert {int(row['ssd_id']) for row in event} == set(range(ssd_count))
        assert len(event) == ssd_count
    for sequence, record in enumerate(records):
        recorded = [row for row in rows if int(row['migration_sequence']) == sequence]
        assert sum(row['event'] == 'migration_start' for row in recorded) == ssd_count
        if record.findtext('Status') != 'ACTIVE':
            assert sum(row['event'] in ('migration_end', 'migration_abort') for row in recorded) == ssd_count
    return rows


def check_epoch_handoff(binary, folder):
    folder.mkdir()
    period = 10000000
    device = ET.parse(ROOT / 'ssdconfig.xml')
    for key, value in {
        'SSD_Count': 2, 'Enabled_Preconditioning': 'false',
        'Flash_Channel_Count': 1, 'Chip_No_Per_Channel': 1,
        'Die_No_Per_Chip': 1, 'Plane_No_Per_Die': 1, 'Block_No_Per_Plane': 128,
        'Page_No_Per_Block': 8, 'Page_Capacity': 4096, 'Stripe_Unit_LBA': 64,
        'SWANS_Zone_Size_LBA': 64, 'SWANS_Enabled': 'true', 'Data_Cache_Capacity': 0,
        'op_zone_migration': 'false', 'SWANS_Migration_Mode': 'ON_DEMAND',
        'SWANS_State_Evaluation': 'WRITE_STDDEV', 'SWANS_Zone_Selection': 'CUMULATIVE',
        'SWANS_Max_Concurrent_Migrations': 1, 'SWANS_Balance_Unit_Bytes': 4096,
        'SWANS_TH_Precautionary': 1, 'SWANS_TH_Critical': 2,
        'SWANS_Epoch_Migration': period,
        'RAID_Telemetry_Enabled': 'false',
    }.items():
        setting(device, key, value)
    device.write(folder / 'ssd.xml')
    trace = folder / 'io.trace'
    # t=10 ms starts zone 0. At 20 ms it still wins. At 30 ms the
    # write imbalance is below threshold. At 40 ms zone 2 takes over.
    # Zone 4 heats up just after the old 50 ms tick, before the rebased tick.
    trace.write_text(
        '1000000 0 0 32 0\n'
        '21000000 0 192 24 0\n'
        '31000000 0 128 48 0\n'
        '50100000 0 256 64 0\n'
        '65000000 0 256 8 1\n', encoding='ascii')
    workload = ET.Element('MQSim_IO_Scenarios')
    flow = ET.SubElement(ET.SubElement(workload, 'IO_Scenario'), 'IO_Flow_Parameter_Set_Trace_Based')
    for key, value in {
        'Priority_Class': 'HIGH', 'Device_Level_Data_Caching_Mode': 'TURNED_OFF',
        'Channel_IDs': 0, 'Chip_IDs': 0, 'Die_IDs': 0, 'Plane_IDs': 0,
        'Initial_Occupancy_Percentage': 0, 'File_Path': trace.as_posix(),
        'Percentage_To_Be_Executed': 100, 'Relay_Count': 1, 'Time_Unit': 'NANOSECOND',
    }.items():
        ET.SubElement(flow, key).text = str(value)
    ET.ElementTree(workload).write(folder / 'workload.xml')
    run = subprocess.run([str(binary), '-i', 'ssd.xml', '-w', 'workload.xml'], cwd=folder,
                         input='\n', text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    (folder / 'run.log').write_text(run.stdout, encoding='utf-8')
    assert run.returncode == 0, run.stdout[-2000:]
    report = ET.parse(folder / 'workload_scenario_1.xml').getroot()
    def value(key): return next(report.iter(key)).text
    records = [node for node in report.iter() if node.tag.startswith('SWANS_Migration_Record_')]
    assert [r.findtext('Hot_Zone') for r in records] == ['0', '2', '4']
    first, second, third = records
    starts = [int(r.findtext('Start_Time')) for r in records]
    assert starts[0] == period
    assert 4 * period < starts[1] < 5 * period and starts[1] == int(first.findtext('End_Time'))
    assert starts[1] + period < starts[2] < 6 * period and starts[2] == int(second.findtext('End_Time'))
    assert third.findtext('Status') == 'ACTIVE'
    assert value('SWANS_Lazy_Handoff_Drains') == '2'
    assert value('SWANS_Lazy_Deadline_Drains') == '0'
    assert int(value('SWANS_Normal_Epochs')) >= 1
    assert int(value('SWANS_Migration_Epochs')) >= 4
    assert value('SWANS_Mapping_Duplicate_Physical_Locations') == '0'
    assert value('Termination_Reason') == 'EVENT_QUEUE_EMPTY'
    rows = check_migration_csv(folder, report)
    assert sum(row['event'] == 'migration_force' for row in rows) == 4
    assert all(row['force_reason'] == 'TARGET_CHANGED' for row in rows if row['event'] == 'migration_force')
    assert all(float(row['seconds_since_previous_end']) == 0 for row in rows
               if row['event'] == 'migration_start' and row['previous_migration_sequence'])
    print(f'Lazy epoch handoffs and clock reset passed: {starts}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'MQSim.exe')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/lazy-validation')
    args = parser.parse_args()
    binary, output = args.binary.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    check_epoch_handoff(binary, output / 'epoch-handoff')
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
        check_migration_csv(folder, report)
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
            check_lazy_start_times(records, PERIOD)
            assert int(value('SWANS_Lazy_Offloaded_Pages')) > 0
            assert value('SWANS_Lazy_Deadline_Drains') == '0'
        if reserve:
            assert all(e.findtext('Mode') == 'op_zone_migration' for e in records)
            assert int(value('SWANS_Exchange_Cold_Stages_Completed')) >= len(completed)
        result = dict(test=name,completed_migrations=len(completed),offloaded_pages=int(value('SWANS_Lazy_Offloaded_Pages')),
                      copied_pages=int(value('SWANS_Lazy_Copied_Pages')),handoff_drains=int(value('SWANS_Lazy_Handoff_Drains')),
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
            conflict_rows = check_migration_csv(folder, conflict, 'conflict')
            assert any(row['event'] == 'migration_force' and row['force_reason'] == 'COLD_WRITE'
                       for row in conflict_rows)
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
