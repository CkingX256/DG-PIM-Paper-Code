from pathlib import Path
import tempfile,subprocess,tarfile,os,json,yaml,copy,resource,argparse
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
R=Path(__file__).resolve().parents[3];C=R/'output/consumer';sample=C/'adversarial_native/candidate__max_migration/raw.tar.gz';rows=[]
ap=argparse.ArgumentParser();ap.add_argument('--build',default='build_final');ap.add_argument('--prefix',default='');a=ap.parse_args()
with tarfile.open(sample)as tar:cfg=yaml.safe_load(tar.extractfile('config.yaml').read())
for name,params,expected in [('stale_slot',{'fault':'qpipe_stale_slot'},'stale slot/owner'),('invalid_depth',{'qk_pipeline_depth':3},'Invalid QK pipeline capacity'),('unfunded_cache',{'qk_reuse_pipeline':False},'requires its charged non-inplace'),('unknown_policy',{'qk_exp_policy':'oracle'},'Unknown exact cache admission')]:
 with tempfile.TemporaryDirectory(prefix='consumer_fault_',dir='/dev/shm')as td:
  td=Path(td);current=copy.deepcopy(cfg);m=current['MemorySystem'];m.update(params,output_directory=str(td));m['Controller']['plugins'][0]['ControllerPlugin']['path']=str(td/'commands.log');path=td/'config.yaml';path.write_text(yaml.safe_dump(current));binary=C/a.build/'bin/ramulator2';p=subprocess.run(['/lib64/ld-linux-x86-64.so.2',str(binary),'-f',str(path)],env=dict(os.environ,LD_LIBRARY_PATH=str(binary.parent)),capture_output=True,text=True,timeout=60)
  assert p.returncode!=0 and expected in p.stderr,(name,p.returncode,p.stderr[-1500:]);d=C/(a.prefix+'fault_controls')/name;d.mkdir(parents=True,exist_ok=True);(d/'config.yaml').write_text(path.read_text());(d/'stderr.log').write_text(p.stderr);rows.append(dict(name=name,status='EXPECTED_REJECTION',returncode=p.returncode,expected_error=expected))
(C/'results'/(a.prefix+'FAULT_CONTROLS.json')).write_text(json.dumps(dict(status='ALL4_INVALID_CONFIG_OR_OWNER_CONTROLS_REJECTED',rows=rows),indent=2));print('FAULT_CONTROLS_PASS',a.build,len(rows))
