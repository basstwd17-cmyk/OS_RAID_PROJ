"""Exercise 2 state evaluators x 2 reserve settings x 2 migration executors on real NAND."""
from pathlib import Path
import argparse
import itertools
import json
import random
import subprocess
import xml.etree.ElementTree as ET
from validate_lazy import setting

ROOT = Path(__file__).resolve().parents[1]
PERIOD = 100000000


def put(root, key, value):
    node = root.find(key)
    if node is None:
        node = ET.SubElement(root, key)
    node.text = str(value)


def inputs(folder, state, reserve, mode, redirects=False):
    folder.mkdir()
    device = ET.parse(ROOT / 'ssdconfig.xml')
    for key, value in {
        'SSD_Count':4, 'Enabled_Preconditioning':'false', 'Flash_Channel_Count':1,
        'Chip_No_Per_Channel':1, 'Die_No_Per_Chip':1, 'Plane_No_Per_Die':1,
        'Block_No_Per_Plane':128, 'Page_No_Per_Block':8, 'Page_Capacity':4096,
        'Block_PE_Cycles_Limit':3, 'Overprovisioning_Ratio':.3, 'End_of_Life_Threshold':.05,
        'GC_Exec_Threshold':.15, 'IO_Queue_Depth':16, 'Queue_Fetch_Size':16,
        'Stripe_Unit_LBA':64, 'Data_Cache_Capacity':0,
        'RAID_Telemetry_Enabled':'true', 'RAID_Telemetry_Period':PERIOD,
        # Deliberately conflicting legacy fields: the new selector must win.
        'SWANS_Wear_Metric':'WRITE_BYTES' if state=='TEC_PWL' else 'TEC',
        'SWANS_Migration_Trigger':'IMBALANCE' if state=='TEC_PWL' else 'DYNAMIC_WEAR',
    }.items():
        setting(device,key,value)
    device.write(folder/'ssd.xml')
    preset = ('stddev' if state=='WRITE_STDDEV' else 'tec-pwl')+'-extra-'+('on' if reserve else 'off')+'-'+('eager' if mode=='EAGER' else 'lazy')+'.xml'
    policy = ET.parse(ROOT/'examples/policy-options'/preset)
    for key, value in {
        'SWANS_Zone_Size_LBA':64, 'SWANS_Balance_Unit_Bytes':4096,
        'SWANS_Epoch_Default':PERIOD, 'SWANS_Epoch_Placement':PERIOD,
        'SWANS_Epoch_Migration':PERIOD, 'SWANS_Zone_Popularity_Period':PERIOD,
        'SWANS_Migration_Working_Queue_Limit':8,
        'SWANS_TH_Precautionary':1 if redirects else 8,
        'SWANS_TH_Critical':1000000000 if redirects else 32,
        'SWANS_TEC_TH_Precautionary_PctPoint':1 if redirects else 40,
        'SWANS_TEC_TH_Critical_PctPoint':45,
    }.items():
        put(policy.getroot(),key,value)
    policy.write(folder/'policy.xml')
    rng = random.Random(49)
    rows = []
    if reserve and not redirects:
        rows += [(z*64,8,0) for z in range(88*4)]
    for i in range(6000):
        if redirects:
            z = ((i//200+1)%60)*4 if i%200==0 else 0
            rows.append((z*64+rng.randrange(8)*8,8,0))
        else:
            z = 0 if i%5==0 else rng.randrange(72)*4
            if reserve and i%19==0:
                z = (i//19%88)*4+1+i//19%3
            partial = i%7==0
            rows.append((z*64+rng.randrange(8)*8+(2 if partial else 0),4 if partial else 8,1 if i%11==0 else 0))
    trace = folder/'io.trace'
    trace.write_text(''.join(f'{(i+1)*2000000} 0 {lba} {count} {op}\n' for i,(lba,count,op) in enumerate(rows)),encoding='ascii')
    workload = ET.Element('MQSim_IO_Scenarios')
    flow = ET.SubElement(ET.SubElement(workload,'IO_Scenario'),'IO_Flow_Parameter_Set_Trace_Based')
    for key,value in {
        'Priority_Class':'HIGH', 'Device_Level_Data_Caching_Mode':'TURNED_OFF',
        'Channel_IDs':0,'Chip_IDs':0,'Die_IDs':0,'Plane_IDs':0,
        'Initial_Occupancy_Percentage':0,'File_Path':trace.as_posix(),
        'Percentage_To_Be_Executed':100,'Relay_Count':0,'Time_Unit':'NANOSECOND',
    }.items():
        put(flow,key,value)
    ET.ElementTree(workload).write(folder/'workload.xml')


def run(binary, folder):
    result = subprocess.run([str(binary),'-i',str(folder/'ssd.xml'),'-w',str(folder/'workload.xml'),'-s',str(folder/'policy.xml')],
                            cwd=folder,input='\n',text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
    (folder/'run.log').write_text(result.stdout,encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,default=ROOT/('MQSim.exe' if (ROOT/'MQSim.exe').exists() else 'MQSim'))
    parser.add_argument('--output',type=Path,default=ROOT/'build/policy-options-validation')
    args = parser.parse_args()
    binary, output = args.binary.resolve(), args.output.resolve()
    output.mkdir(parents=True,exist_ok=False)
    results = []
    cases = list(itertools.product(['WRITE_STDDEV','TEC_PWL'],[False,True],['EAGER','ON_DEMAND']))
    for redirects in [False,True]:
        for state,reserve,mode in cases:
            name = f'{state}-{reserve}-{mode}'+('-redirect' if redirects else '')
            folder = output/name
            inputs(folder,state,reserve,mode,redirects)
            proc = run(binary,folder)
            assert proc.returncode==0,(name,proc.stdout[-3000:])
            report = ET.parse(folder/'workload_scenario_1.xml').getroot()
            def value(key): return next(report.iter(key)).text
            assert value('Termination_Reason')=='EOL',name
            assert value('SWANS_State_Evaluation')==state,name
            assert value('SWANS_Migration_Mode')==mode,name
            assert value('SWANS_Extra_Zone_Enabled')==str(reserve).lower(),name
            assert value('SWANS_Redirect_Enabled')=='true',name
            assert value('SWANS_Mu_Unit')==('percent_points' if state=='TEC_PWL' else 'write_bytes_normalized_by_balance_unit'),name
            assert int(value('SWANS_Reserve_Zone_Count'))==(4 if reserve else 0),name
            assert value('SWANS_Mapping_Duplicate_Physical_Locations')=='0',name
            records = [node for node in report.iter() if node.tag.startswith('SWANS_Migration_Record_')]
            completed = [node for node in records if node.findtext('Status')=='COMPLETED']
            if redirects:
                assert int(value('SWANS_Redirect_Epochs'))>0,name
                assert int(value('SWANS_Redirect_Operations'))>0,(name,'no actual redirect')
            else:
                assert completed,(name,'no completed migration')
                assert all((r.findtext('Reserve_Zone')!='NONE')==reserve for r in records),name
                if state=='TEC_PWL':
                    assert all(float(r.findtext('Source_Wear'))>float(r.findtext('Wear_Threshold')) for r in records),name
                if mode=='ON_DEMAND':
                    starts = [int(r.findtext('Start_Time')) for r in records]
                    assert len(starts)==len(set(starts)) and all(t%PERIOD==0 for t in starts),name
                    assert int(value('SWANS_Lazy_Offloaded_Pages'))>0,(name,'no write offload')
                    assert int(value('SWANS_Lazy_Deadline_Drains'))>0,(name,'no deadline drain')
                    assert all(r.findtext('Status')!='ACTIVE' for r in records),name
                else:
                    assert value('SWANS_Lazy_Offloaded_Pages')=='0',name
            summary = dict(test=name,completed_migrations=len(completed),redirects=int(value('SWANS_Redirect_Operations')),
                           redirect_epochs=int(value('SWANS_Redirect_Epochs')),offloaded_pages=int(value('SWANS_Lazy_Offloaded_Pages')))
            results.append(summary)
            print(json.dumps(summary),flush=True)

    # Reject selector typos before simulation, then verify the selector in base XML too.
    folder = output/'selector-errors'
    inputs(folder,'TEC_PWL',False,'ON_DEMAND')
    policy = ET.parse(folder/'policy.xml')
    put(policy.getroot(),'SWANS_State_Evaluation','TYPO');policy.write(folder/'policy.xml')
    invalid = run(binary,folder)
    assert invalid.returncode!=0 and 'SWANS_State_Evaluation' in invalid.stdout
    # Root XML and -s must resolve identically, including the explicit selector's precedence.
    device = ET.parse(folder/'ssd.xml')
    put(policy.getroot(),'SWANS_State_Evaluation','WRITE_STDDEV')
    for node in policy.getroot(): setting(device,node.tag,node.text)
    device.write(folder/'ssd.xml')
    ET.ElementTree(ET.Element('SWANS_Parameter_Set')).write(folder/'policy.xml')
    base = run(binary,folder)
    assert base.returncode==0,base.stdout[-1500:]
    report = ET.parse(folder/'workload_scenario_1.xml').getroot()
    assert next(report.iter('SWANS_State_Evaluation')).text=='WRITE_STDDEV'
    (output/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    print('Selector validation and base-XML parsing passed',flush=True)


if __name__=='__main__':
    main()
