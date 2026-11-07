"""Actual NAND EOL runs; deterministic pool replay and TEC snapshots/policy wiring."""
from pathlib import Path
import csv
import random
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1]).resolve()
OUT = ROOT / 'build' / 'advanced-smoke'
OUT.mkdir(parents=True, exist_ok=True)

def run(name, metric='TEC', trigger='DYNAMIC_WEAR', enabled=True, invalid=False, defaults=False, precautionary=40, critical=45, preconditioning=False, overlay=False):
    folder = OUT / name
    folder.mkdir(exist_ok=True)
    trace_dir = folder / 'traces'
    trace_dir.mkdir(exist_ok=True)
    rng = random.Random(49)
    for f in range(3):
        with (trace_dir / f'{f}.trace').open('w') as out:
            for i in range(400):
                lba = rng.randrange(72) * 4 * 64 + rng.randrange(8) * 8
                out.write(f'{900000000 + i * 2000000} 0 {lba} 8 0\n')
    tree = ET.parse(ROOT / 'ssdconfig.xml')
    device = tree.find('.//Device_Parameter_Set')
    settings = {
        'SSD_Count':4, 'Enabled_Preconditioning':str(preconditioning).lower(),
        'Flash_Channel_Count':1, 'Chip_No_Per_Channel':1, 'Die_No_Per_Chip':1, 'Plane_No_Per_Die':1,
        'Block_No_Per_Plane':128, 'Page_No_Per_Block':8, 'Block_PE_Cycles_Limit':3,
        'Overprovisioning_Ratio':0.3, 'End_of_Life_Threshold':0.05, 'GC_Exec_Threshold':0.15,
        'Stripe_Unit_LBA':64, 'SWANS_Zone_Size_LBA':64, 'SWANS_Reserve_Zone_Enabled':'false',
        'SWANS_Epoch_Default':1000000, 'SWANS_Epoch_Placement':1000000, 'SWANS_Epoch_Migration':1000000,
        'SWANS_Zone_Popularity_Period':40000000000, 'SWANS_TH_Precautionary':1e12,
        'SWANS_TH_Critical':1e13, 'SWANS_Enabled':str(enabled).lower(), 'SWANS_Wear_Metric':metric,
        'SWANS_Migration_Trigger':trigger, 'SWANS_Zone_Selection':'SSD_POPULARITY',
        'SWANS_TEC_TH_Precautionary_PctPoint':precautionary, 'SWANS_TEC_TH_Critical_PctPoint':critical,
        'RAID_Telemetry_Enabled':'true', 'RAID_Telemetry_Period':1000000000,
        'IO_Queue_Depth':16, 'Queue_Fetch_Size':16, 'Bad_Block_Retirement_Enabled':'true',
    }
    for key,value in settings.items():
        node=tree.find('.//'+key)
        if node is None: node=ET.SubElement(device,key)
        node.text=str(value)
    if defaults:
        device.remove(device.find('SWANS_Wear_Metric'))
        device.remove(device.find('SWANS_Migration_Trigger'))
    if invalid:
        device.remove(device.find('SWANS_TEC_TH_Precautionary_PctPoint'))
        device.remove(device.find('SWANS_TEC_TH_Critical_PctPoint'))
    overlay_args=[]
    if overlay:
        swans=ET.Element('SWANS_Parameter_Set')
        for key in ['SWANS_Wear_Metric','SWANS_Migration_Trigger','SWANS_Zone_Selection',
                    'SWANS_TEC_TH_Precautionary_PctPoint','SWANS_TEC_TH_Critical_PctPoint']:
            node=device.find(key)
            ET.SubElement(swans,key).text=node.text
            device.remove(node)
        ET.ElementTree(swans).write(folder/'policy.xml')
        overlay_args=['-s',str(folder/'policy.xml')]
    tree.write(folder/'ssd.xml')
    scenario=ET.Element('MQSim_IO_Scenarios')
    flow=ET.SubElement(ET.SubElement(scenario,'IO_Scenario'),'IO_Flow_Parameter_Set_Trace_Based')
    fields={'Priority_Class':'HIGH','Device_Level_Data_Caching_Mode':'TURNED_OFF','Channel_IDs':0,'Chip_IDs':0,
            'Die_IDs':0,'Plane_IDs':0,'Initial_Occupancy_Percentage':0,'File_Path':'',
            'Trace_Directory':trace_dir.as_posix(),'Trace_Selection_Seed':42,'Trace_Inter_File_Gap_NS':2000000,
            'Relay_Count':0,'Percentage_To_Be_Executed':100,'Time_Unit':'NANOSECOND'}
    for key,value in fields.items(): ET.SubElement(flow,key).text=str(value)
    ET.ElementTree(scenario).write(folder/'workload.xml')
    result=subprocess.run([str(BINARY),'-i',str(folder/'ssd.xml'),'-w',str(folder/'workload.xml')]+overlay_args,
                          input='\n',text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,cwd=folder,timeout=90)
    (folder/'run.log').write_text(result.stdout,encoding='utf-8')
    if invalid:
        assert result.returncode and 'TEC requires explicit' in result.stdout
        print(name+': invalid thresholds rejected'); return
    if preconditioning:
        assert result.returncode and 'Trace_Directory requires Enabled_Preconditioning=false' in result.stdout
        print(name+': unsupported preconditioning rejected before simulation'); return
    assert result.returncode==0,result.stdout[-2500:]
    xml=ET.parse(folder/'workload_scenario_1.xml').getroot()
    def find(suffix): return next(e for e in xml.iter() if e.tag.endswith(suffix))
    assert next(e.text for e in xml.iter() if e.tag=='Termination_Reason')=='EOL'
    snapshot=find('.EOLSnapshot')
    ssds=[e for e in snapshot if e.tag.endswith('.SSD')]
    assert len(ssds)==4
    for s in ssds:
        assert s.findtext('TEC_Valid')=='true'
        assert int(s.findtext('TEC_User_Block_Boundary'))==90
        assert abs(float(s.findtext('Wear_Percent'))-100*int(s.findtext('TEC'))/3)<1e-5
        assert float(s.findtext('TEC_Boundary_Minus_Logical_Blocks'))>=0
    assert any(int(s.findtext('TEC'))>0 for s in ssds)
    # Independent full histogram, including retired blocks, checks cache freshness at EOL.
    histograms=[e for e in xml.iter() if e.tag.endswith('.EraseHistogram.SSD')]
    for h in histograms:
        counts=[]
        for bin in h:
            if bin.tag.endswith('.Bin'):
                counts.extend([int(bin.findtext('Erase_Count'))]*int(bin.findtext('Block_Count')))
        assert len(counts)==128
        s=ssds[int(h.findtext('SSD_ID'))]
        assert sorted(counts)[90]==int(s.findtext('TEC'))
    pool_result=find('.TracePool')
    assert pool_result.findtext('Seed')=='42' and len(pool_result.findall('Trace_File'))==3
    assert any(float(s.findtext('Remaining_OP_Ratio'))<=0.05 for s in ssds)
    policy=find('.SWANSPolicyStatistics')
    assert policy.findtext('Mu_Unit')==('percent_points' if metric=='TEC' else 'write_units')
    records=[e for e in xml.iter() if e.tag.startswith('SWANS_Migration_Record_')]
    if enabled and metric=='TEC':
        assert records, 'expected actual TEC migration'
    if enabled and metric=='TEC' and trigger=='DYNAMIC_WEAR':
        assert records,'expected actual TEC-triggered NAND migration'
        for r in records:
            assert float(r.findtext('Source_Wear'))>float(r.findtext('Wear_Threshold'))
    rows=list(csv.DictReader((folder/'workload_scenario_1.telemetry.csv').open()))
    assert any(r['event']=='eol' and float(r['wear_percent'])>0 for r in rows)
    rounds=re.findall(r'TRACE_POOL round=(\d+) file=.*[/\\]([^/\\ ]+\.trace) start_ns=(\d+)',result.stdout)
    assert len(rounds)>3 and len({r[1] for r in rounds})>1
    assert int(rounds[0][2])==0 and all(int(a[2])<int(b[2]) for a,b in zip(rounds,rounds[1:]))
    print(f'{name}: EOL after {len(rounds)} trace segments; {len(records)} migrations; TEC/OP verified')
    return rounds,snapshot.findtext('Completed_Host_Write_Bytes')

a=run('tec-dynamic')
b=run('tec-repeat',overlay=True)
assert a==b,'same seed/config must reproduce selection sequence and completed EOL writes'
run('tec-imbalance',trigger='IMBALANCE',precautionary=5,critical=10)
run('write-observation',metric='WRITE_BYTES',trigger='IMBALANCE',defaults=True)
run('swans-off',metric='WRITE_BYTES',trigger='IMBALANCE',enabled=False)
run('missing-thresholds',invalid=True)
run('pool-preconditioning',metric='WRITE_BYTES',trigger='IMBALANCE',enabled=False,preconditioning=True)
print('advanced SWANS simulator smoke tests passed')
