"""Exercise independent OP/TEC stopping rules through real NAND erases."""
from pathlib import Path
import random
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1]).resolve()
OUT = ROOT / 'build' / 'eol-modes'
OUT.mkdir(parents=True, exist_ok=True)

def run(name, mode=None, tec=100, retirement=True, expected=None):
    folder = OUT / name
    folder.mkdir(exist_ok=True)
    traces = folder / 'traces'
    traces.mkdir(exist_ok=True)
    rng = random.Random(49)
    with (traces / 'input.trace').open('w') as f:
        for i in range(1200):
            lba = rng.randrange(72) * 4 * 64 + rng.randrange(8) * 8
            f.write(f'{i * 2000000} 0 {lba} 8 0\n')
    tree = ET.parse(ROOT / 'ssdconfig.xml')
    device = tree.find('.//Device_Parameter_Set')
    settings = {
        'SSD_Count':4, 'Enabled_Preconditioning':'false',
        'Flash_Channel_Count':1, 'Chip_No_Per_Channel':1, 'Die_No_Per_Chip':1,
        'Plane_No_Per_Die':1, 'Block_No_Per_Plane':128, 'Page_No_Per_Block':8,
        'Block_PE_Cycles_Limit':3, 'Overprovisioning_Ratio':0.3,
        'End_of_Life_Threshold':0.20, 'End_of_Life_TEC_Threshold_Percent':tec,
        'GC_Exec_Threshold':0.15, 'Stripe_Unit_LBA':64, 'SWANS_Zone_Size_LBA':64,
        'SWANS_Enabled':'false', 'SWANS_Wear_Metric':'WRITE_BYTES',
        'Bad_Block_Retirement_Enabled':str(retirement).lower(),
        'IO_Queue_Depth':16, 'Queue_Fetch_Size':16, 'RAID_Telemetry_Enabled':'true',
    }
    if mode is not None: settings['End_of_Life_Mode'] = mode
    else:
        node = device.find('End_of_Life_Mode')
        if node is not None: device.remove(node)
    for key, value in settings.items():
        node = tree.find('.//' + key)
        if node is None: node = ET.SubElement(device, key)
        node.text = str(value)
    tree.write(folder / 'ssd.xml')
    workload = ET.parse(ROOT / 'examples/advanced-swans/trace-pool-workload.xml')
    workload.find('.//Trace_Directory').text = str(traces)
    workload.write(folder / 'workload.xml')
    result = subprocess.run([str(BINARY), '-i', str(folder/'ssd.xml'), '-w', str(folder/'workload.xml')],
                            cwd=folder, input='\n', text=True, capture_output=True, timeout=60)
    (folder/'run.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    if expected:
        assert result.returncode != 0 and expected in result.stdout + result.stderr
        print(name + ': rejected'); return
    assert result.returncode == 0, result.stdout[-2000:] + result.stderr
    root = ET.parse(folder/'workload_scenario_1.xml').getroot()
    assert next(root.iter('Termination_Reason')).text == 'EOL'
    snap = next(e for e in root.iter() if e.tag.endswith('.EOLSnapshot'))
    ssds = [e for e in snap if e.tag.endswith('.SSD')]
    wear = max(float(s.findtext('Wear_Percent')) for s in ssds)
    op = min(float(s.findtext('Remaining_OP_Ratio')) for s in ssds)
    assert snap.findtext('End_of_Life_Mode') == (mode or 'OP_RATIO')
    if mode == 'TEC':
        assert wear >= float(tec) and wear - 100/3 < float(tec) + 1e-5
        if float(tec) == 100 and retirement: assert op < .20 # OP condition did not stop TEC mode
        assert abs(float(snap.findtext('End_of_Life_Active_Threshold')) - float(tec)) < 1e-6
        if not retirement: assert all(s.findtext('Bad_Block_Count') == '0' for s in ssds)
    else:
        assert op <= .20 and op + 1/128 > .20 # stop at first crossing
    print(f'{name}: wear={wear:.6f}%, remaining OP={op:.6f}')
    return snap.findtext('Completed_Host_Write_Bytes'), wear, op

legacy = run('legacy')
assert legacy == run('op-explicit', 'OP_RATIO')
assert legacy == run('op-ignores-tec', 'OP_RATIO', tec=30)
run('tec-100', 'TEC')
run('tec-before-retirement', 'TEC', tec=30, retirement=False)
run('invalid-mode', 'UNKNOWN', expected='End_of_Life_Mode must')
for value in [0, -1, 101, 'nan']:
    run('invalid-tec-' + str(value), 'TEC', tec=value, expected='must be in (0, 100]')
for i, value in enumerate(['30garbage', '30%', '30,5', '3e1junk', '30 40']):
    run('invalid-tec-token-' + str(i), 'TEC', tec=value, expected='Error in Device_Parameter_Set')
for i, value in enumerate(['30.5', '3e1', ' \t30\n ', '+30', '0.1', '100']):
    run('valid-tec-token-' + str(i), 'TEC', tec=value, retirement=False)
print('EOL mode tests passed')
