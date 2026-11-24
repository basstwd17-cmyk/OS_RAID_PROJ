"""Check unmapped-read policy, host completion, NAND reads and zone validity."""
import argparse
import subprocess
import shutil
from pathlib import Path
import xml.etree.ElementTree as ET

from validate_lazy import ROOT, setting, check_epoch_handoff


def run_case(binary, folder, policy, rows, valid_sectors, nand_reads, swans=True,
             ideal=True, cache='TURNED_OFF'):
    folder.mkdir()
    device = ET.parse(ROOT / 'ssdconfig.xml')
    values = {
        'SSD_Count': 2, 'Enabled_Preconditioning': 'false', 'Flash_Channel_Count': 1,
        'Chip_No_Per_Channel': 1, 'Die_No_Per_Chip': 1, 'Plane_No_Per_Die': 1,
        'Block_No_Per_Plane': 128, 'Page_No_Per_Block': 8, 'Page_Capacity': 4096,
        'Stripe_Unit_LBA': 64, 'SWANS_Zone_Size_LBA': 64, 'SWANS_Enabled': str(swans).lower(),
        'SWANS_Migration_Mode': 'EAGER', 'SWANS_State_Evaluation': 'WRITE_STDDEV',
        'SWANS_TH_Precautionary': 100000, 'SWANS_TH_Critical': 200000,
        'Ideal_Mapping_Table': str(ideal).lower(), 'RAID_Telemetry_Enabled': 'true',
        'Unmapped_Read_Policy': policy or 'INITIALIZE',
    }
    for key, value in values.items():
        setting(device, key, value)
    if policy is None:
        parameters = device.find('.//Device_Parameter_Set')
        parameters.remove(parameters.find('Unmapped_Read_Policy'))
    device.write(folder / 'ssd.xml')
    trace = folder / 'io.trace'
    trace.write_text(''.join(f'{(i+1)*10000000} 0 {lba} {count} {op}\n'
                             for i, (lba, count, op) in enumerate(rows)), encoding='ascii')
    workload = ET.Element('MQSim_IO_Scenarios')
    flow = ET.SubElement(ET.SubElement(workload, 'IO_Scenario'), 'IO_Flow_Parameter_Set_Trace_Based')
    for key, value in {
        'Priority_Class': 'HIGH', 'Device_Level_Data_Caching_Mode': cache,
        'Channel_IDs': 0, 'Chip_IDs': 0, 'Die_IDs': 0, 'Plane_IDs': 0,
        'Initial_Occupancy_Percentage': 0, 'File_Path': trace.as_posix(),
        'Percentage_To_Be_Executed': 100, 'Relay_Count': 1, 'Time_Unit': 'NANOSECOND',
    }.items():
        ET.SubElement(flow, key).text = str(value)
    ET.ElementTree(workload).write(folder / 'workload.xml')
    process = subprocess.run([str(binary), '-i', 'ssd.xml', '-w', 'workload.xml'],
                             cwd=folder, input='\n', text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=20)
    (folder / 'run.log').write_text(process.stdout)
    assert process.returncode == 0, process.stdout[-2000:]
    report = ET.parse(folder / 'workload_scenario_1.xml').getroot()
    def first(key): return next(report.iter(key)).text
    assert first('Termination_Reason') == 'EVENT_QUEUE_EMPTY'
    assert int(first('Completed_Host_Requests')) == len(rows)
    assert int(first('Completed_Read_Sectors')) == sum(size for _, size, op in rows if op == 1)
    actual_reads = sum(int(node.text) for node in report.iter('Completed_Read_Transactions'))
    assert actual_reads == nand_reads, (folder.name, actual_reads, nand_reads)
    assert set(node.text for node in report.iter('Unmapped_Read_Policy')) == {policy or 'INITIALIZE'}
    if swans:
        assert int(first('SWANS_Valid_User_Sectors')) == valid_sectors
        if valid_sectors == 0:
            assert first('SWANS_Empty_User_Zones') == first('SWANS_Mapping_Entries')
    print(f'{folder.name}: host={len(rows)}, NAND reads={actual_reads}, valid sectors={valid_sectors}', flush=True)
    return folder / 'ssd.xml'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'MQSim')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    binary, output = args.binary.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    for policy in ('INITIALIZE', 'IGNORE', None):
        label = policy or 'default'
        for swans in (False, True):
            for ideal in (False, True):
                name = f'{label}-swans-{swans}-ideal-{ideal}'
                initialize = policy != 'IGNORE'
                run_case(binary, output / name, policy, [(0, 8, 1), (0, 8, 1)],
                         8 if initialize else 0, 2 if initialize else 0, swans, ideal)
        run_case(binary, output / f'{label}-mapped-empty-sectors', policy,
                 [(0, 2, 0), (2, 2, 1)], 4 if initialize else 2, 1)
        run_case(binary, output / f'{label}-mixed-pages', policy,
                 [(0, 2, 0), (0, 16, 1)], 16 if initialize else 2, 2 if initialize else 1)
        config = run_case(binary, output / f'{label}-mixed-ssds', policy,
                          [(56, 8, 0), (56, 16, 1)], 16 if initialize else 8, 2 if initialize else 1)
    device = ET.parse(config)
    setting(device, 'Unmapped_Read_Policy', 'INVALID')
    invalid = config.parent / 'invalid.xml'
    device.write(invalid)
    process = subprocess.run([str(binary), '-i', str(invalid), '-w', str(config.parent / 'workload.xml')],
                             input='\n', text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
    assert process.returncode != 0 and 'Unmapped_Read_Policy' in process.stdout
    print('Invalid policy rejected', flush=True)
    for cache in ('WRITE_CACHE', 'WRITE_READ_CACHE'):
        run_case(binary, output / f'IGNORE-cache-{cache}', 'IGNORE',
                 [(0, 8, 1), (0, 8, 1)], 0, 0, swans=False, cache=cache)
    check_epoch_handoff(binary, output / 'lazy-handoff-ignore', policy='IGNORE')
    # A cold logical destination stays empty while lazy hot writes put physical
    # copies there. Reads must not expose those copies or force the task to drain.
    folder = output / 'lazy-empty-destination'
    folder.mkdir()
    source = output / 'lazy-handoff-ignore'
    shutil.copyfile(source / 'ssd.xml', folder / 'ssd.xml')
    rows = [tuple(map(int, line.split())) for line in (source / 'io.trace').read_text().splitlines()]
    rows += [(12000000, 0, 0, 8, 0), (15000000, 0, 64, 8, 1)]
    (folder / 'io.trace').write_text(''.join(' '.join(map(str, row)) + '\n' for row in sorted(rows)))
    workload = ET.parse(source / 'workload.xml')
    workload.find('.//File_Path').text = (folder / 'io.trace').as_posix()
    workload.write(folder / 'workload.xml')
    process = subprocess.run([str(binary), '-i', 'ssd.xml', '-w', 'workload.xml'], cwd=folder,
                             input='\n', text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    (folder / 'run.log').write_text(process.stdout)
    assert process.returncode == 0, process.stdout[-2000:]
    report = ET.parse(folder / 'workload_scenario_1.xml').getroot()
    assert int(next(report.iter('Completed_Host_Requests')).text) == len(rows)
    assert next(report.iter('RAID_Ignored_Unmapped_Read_Sectors')).text == '8'
    background_reads = int(next(report.iter('SWANS_Background_Read_IOs')).text)
    assert sum(int(node.text) for node in report.iter('Completed_Read_Transactions')) == background_reads + 1
    records = [node for node in report.iter() if node.tag.startswith('SWANS_Migration_Record_')]
    assert records[0].findtext('Force_Reason') == 'TARGET_CHANGED'
    assert int(records[0].findtext('Force_Time_ns')) > 15000000
    print('Lazy empty-destination read ignored without exposing copied hot data', flush=True)


if __name__ == '__main__':
    main()
